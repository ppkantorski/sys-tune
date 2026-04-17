#include "music_player.hpp"

#include "../tune_result.hpp"
#include "../tune_service.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "aud_wrapper.h"
#include "config/config.hpp"
#include "source.hpp"
#include "resamplers/SDL_audioEX.h"

#include <cstring>
#include <nxExt.h>
#include <strings.h>

namespace tune::impl {

    namespace {
        constexpr float VOLUME_MAX = 1.f;
        constexpr auto PLAYLIST_ENTRY_MAX = 300; // 75k
        constexpr auto PATH_SIZE_MAX = 256;

        struct PlaylistID {
            u32 id{UINT32_MAX};

            bool IsValid() const {
                return id != UINT32_MAX;
            }

            void Reset() {
                id = UINT32_MAX;
            }
        };

        class PlayList {
        public:
            void Init() {
                Clear();
                m_playlist.reserve(PLAYLIST_ENTRY_MAX);
                m_shuffle_playlist.reserve(PLAYLIST_ENTRY_MAX);
            }

            bool Add(const char* path, EnqueueType type) {
                u32 index;
                if (!FindNextFreeEntry(index)) {
                    return false;
                }

                if (!m_entries[index].Add(path)) {
                    return false;
                }

                if (type == EnqueueType::Front) {
                    m_playlist.emplace(m_playlist.cbegin(), index);
                } else {
                    m_playlist.emplace_back(index);
                }

                // add new entry id to shuffle_playlist_list
                const auto shuffle_playlist_size = m_shuffle_playlist.size() + 1;
                const auto shuffle_index = randomGet64() % shuffle_playlist_size;
                m_shuffle_playlist.emplace(m_shuffle_playlist.cbegin() + shuffle_index, index);

                return true;
            }

            bool Remove(u32 index, ShuffleMode shuffle) {
                const auto entry = Get(index, shuffle);
                R_UNLESS(entry.IsValid(), false);

                // remove entry.
                m_entries[entry.id].Remove();

                // remove from both playlists.
                if (shuffle == ShuffleMode::On) {
                    m_playlist.erase(m_playlist.begin() + GetIndexFromID(entry, ShuffleMode::Off));
                    m_shuffle_playlist.erase(m_shuffle_playlist.begin() + index);
                } else {
                    m_playlist.erase(m_playlist.begin() + index);
                    m_shuffle_playlist.erase(m_shuffle_playlist.begin() + GetIndexFromID(entry, ShuffleMode::On));
                }

                return true;
            }

            bool Swap(u32 src, u32 dst, ShuffleMode shuffle) {
                if (src >= Size() || dst >= Size()) {
                    return false;
                }

                if (shuffle == ShuffleMode::On) {
                    std::swap(m_shuffle_playlist[src], m_shuffle_playlist[dst]);
                } else {
                    std::swap(m_playlist[src], m_playlist[dst]);
                }

                return true;
            }

            void Shuffle() {
                const auto size = m_shuffle_playlist.size();
                if (!size) {
                    return;
                }

                for (auto& e : m_shuffle_playlist) {
                    const auto index = randomGet64() % size;
                    std::swap(e, m_shuffle_playlist[index]);
                }
            }

            const char* GetPath(u32 index, ShuffleMode shuffle) const {
                return GetPath(Get(index, shuffle));
            }

            const char* GetPath(const PlaylistID& entry) const {
                R_UNLESS(entry.IsValid(), nullptr);

                return m_entries[entry.id].GetPath();
            }

            void Clear() {
                for (u32 i = 0; i < m_entries.size(); i++) {
                    m_entries[i].Remove();
                }

                m_playlist.clear();
                m_shuffle_playlist.clear();
            }

            u32 Size() const {
                return m_playlist.size();
            }

            PlaylistID Get(u32 index, ShuffleMode shuffle) const {
                if (index >= Size()) {
                    return {};
                }

                if (shuffle == ShuffleMode::On) {
                    return m_shuffle_playlist[index];
                } else {
                    return m_playlist[index];
                }
            }

            u32 GetIndexFromID(const PlaylistID& entry, ShuffleMode shuffle) const {
                if (!entry.IsValid()) {
                    return 0;
                }

                std::span list{m_playlist};
                if (shuffle == ShuffleMode::On) {
                    list = m_shuffle_playlist;
                }

                for (u32 i = 0; i < list.size(); i++) {
                    if (list[i].id == entry.id) {
                        return i;
                    }
                }

                return 0;
            }

        private:
            bool FindNextFreeEntry(u32& index) const {
                for (u32 i = 0; i < m_entries.size(); i++) {
                    if (m_entries[i].IsEmpty()) {
                        index = i;
                        return true;
                    }
                }

                return false;
            }

        private:
            struct PlayListNameEntry {
            public:
                // in most cases, the path will not exceed 256 bytes,
                // so this is a reasonable max rather than 0x301.
                bool Add(const char* path) {
                    if (!IsEmpty()) {
                        return false;
                    }

                    if (std::strlen(path) >= sizeof(m_path)) {
                        return false;
                    }

                    std::strcpy(m_path, path);
                    return true;
                }

                bool Remove() {
                    m_path[0] = '\0';
                    return true;
                }

                bool IsEmpty() const {
                    return m_path[0] == '\0';
                }

                const char* GetPath() const {
                    return m_path;
                }

            private:
                char m_path[PATH_SIZE_MAX]{};
            };

        private:
            std::vector<PlaylistID> m_playlist{};
            std::vector<PlaylistID> m_shuffle_playlist{};
            std::array<PlayListNameEntry, PLAYLIST_ENTRY_MAX> m_entries{};
        };

        PlayList g_playlist;

        PlaylistID g_current;
        u32 g_queue_position;

        LockableMutex g_mutex;

        RepeatMode g_repeat   = RepeatMode::All;
        ShuffleMode g_shuffle = ShuffleMode::Off;
        PlayerStatus g_status = PlayerStatus::FetchNext;
        Source *g_source = nullptr;

        float g_title_volume = 1.f;
        float g_default_title_volume = 1.f;
        bool g_use_title_volume = true;

        constexpr auto AUDIO_FREQ          = 48000;
        constexpr auto AUDIO_CHANNEL_COUNT = 2;
        constexpr auto AUDIO_BUFFER_COUNT  = 2;
        constexpr auto AUDIO_LATENCY_MS    = 42;
        constexpr auto AUDIO_BUFFER_SIZE   = AUDIO_FREQ / 1000 * AUDIO_LATENCY_MS * AUDIO_CHANNEL_COUNT;

        AudioOutBuffer g_audout_buffer[AUDIO_BUFFER_COUNT];
        alignas(0x1000) s16 AudioMemoryPool[AUDIO_BUFFER_COUNT][(AUDIO_BUFFER_SIZE + 0xFFF) & ~0xFFF];
        static_assert((sizeof(AudioMemoryPool[0]) % 0x2000) == 0, "Audio Memory pool needs to be page aligned!");

        bool g_should_pause      = false;
        bool g_should_run        = true;

        /* --------------------------------------------------------------
         * User-action tracking for the pause/play policy engine.
         *
         * g_should_pause is written by two classes of actor:
         *
         *   Policy engine — PmdmntThreadFunc applies "Pause On Start"
         *                   and "Pause On Home" rules when titles
         *                   change or focus flips.
         *
         *   User action   — Play() / Pause() / Next() / Prev() /
         *                   Select() triggered from the overlay.
         *
         * User intent is asymmetric:
         *
         *   - Pause is a STRONG signal ("I want silence"). A manual
         *     pause must survive title changes, HOME presses, and
         *     game re-entries until the user explicitly unpauses.
         *
         *   - Play is a WEAK signal ("resume now"). Tapping Play does
         *     NOT veto future auto-pauses — e.g. Pause On Home should
         *     still fire when the user presses HOME after tapping Play.
         *
         * g_user_paused encodes this as a ONE-WAY veto: while set, the
         * policy engine cannot write g_should_pause = false. The policy
         * engine can still write g_should_pause = true freely (those
         * writes align with the user's intent anyway).
         *
         * g_saved_pause_state is the snapshot captured at focus-loss
         * (HOME press). It's a global rather than a local because user
         * actions performed during a HOME visit must update it so that
         * returning to the game restores the user's LATEST intent, not
         * a stale pre-HOME value. */
        bool g_user_paused       = false;
        bool g_saved_pause_state = false;

        /* --------------------------------------------------------------
         * pdmqry-based focus detection.
         *
         * pmdmnt reports which process is FOREGROUND, but when the user
         * presses HOME during a game the game process stays foreground
         * (it's suspended, not closed). So pmdmnt alone cannot tell the
         * difference between "playing the game" and "at HOME with the
         * game in the background".
         *
         * pdmqry reads the system's play-event log, which records
         * InFocus/OutOfFocus transitions separately from process
         * lifecycle. We use it to drive HOME-aware pause/play policy.
         *
         * Credit: masagrator (SaltyNX) posted this approach in the
         * sys-tune GitHub discussion. The cache avoids re-reading the
         * event log on every 10 ms tick — the event-count check is cheap
         * (one IPC round-trip, returns counters) and only when the count
         * increments do we pull the latest 16 events. The event log on
         * NAND can hold thousands of historical entries; pulling only
         * the tail keeps this bounded. */
        bool g_pdmqry_available = false;

        /* Query whether a given application TID is currently out of focus.
         *
         * Returns 0 (success) and sets *outOfFocus when TIDnow maps to a
         * recent AppletId_application event in the play-event log.
         *
         * Returns non-zero when TIDnow is NOT a tracked application
         * (HOME menu, system applet, etc.) or when pdmqry itself failed.
         * *outOfFocus is left unchanged in that case.
         *
         * Cache key is (event_count, TID) so repeated queries for the
         * same tid are one-IPC-round-trip cheap, but a query for a
         * different tid correctly re-reads the log. */
        Result isApplicationOutOfFocus(u64 TIDnow, bool* outOfFocus) {
            static s32  s_last_total_entries = -1;
            static u64  s_last_queried_tid   = 0;
            static bool s_last_is_out        = false;
            static Result s_last_rc          = 1;

            s32 total_entries = 0, start_entry_index = 0, end_entry_index = 0;
            Result rc = pdmqryGetAvailablePlayEventRange(
                &total_entries, &start_entry_index, &end_entry_index);
            if (R_FAILED(rc)) return rc;

            /* Cache hit. */
            if (total_entries == s_last_total_entries && TIDnow == s_last_queried_tid) {
                if (R_SUCCEEDED(s_last_rc))
                    *outOfFocus = s_last_is_out;
                return s_last_rc;
            }
            s_last_total_entries = total_entries;
            s_last_queried_tid   = TIDnow;

            /* Pull only the latest 16 events — bounded tail read. */
            constexpr s32 EVENT_COUNT = 16;
            PdmPlayEvent events[EVENT_COUNT];
            s32 out = 0;
            s32 start = end_entry_index - (EVENT_COUNT - 1);
            if (start < 0) start = 0;

            rc = pdmqryQueryPlayEvent(start, events, EVENT_COUNT, &out);
            if (R_FAILED(rc)) { s_last_rc = rc; return rc; }
            if (out == 0)     { s_last_rc = 1;  return 1;  }

            /* Walk newest -> oldest, find the most recent applet focus
             * event for TIDnow. Retail games always log under their
             * base program_id; mask the low 12 bits to match updates/DLC. */
            int itr = -1;
            for (int i = out - 1; i >= 0; --i) {
                const PdmPlayEvent* event = &events[i];
                if (event->play_event_type != PdmPlayEventType_Applet)
                    continue;
                if (event->event_data.applet.applet_id != AppletId_application)
                    continue;

                union { u32 parts[2]; u64 full; } TID;
                TID.parts[0] = event->event_data.applet.program_id[1];
                TID.parts[1] = event->event_data.applet.program_id[0];

                if (TID.full != TIDnow && TID.full != (TIDnow & ~0xFFFULL))
                    continue;

                itr = i;
                break;
            }
            if (itr == -1) { s_last_rc = 1; return 1; }

            const auto event_type = events[itr].event_data.applet.event_type;
            const bool isOut = (event_type == PdmAppletEventType_OutOfFocus)
                            || (event_type == PdmAppletEventType_OutOfFocus4);
            *outOfFocus   = isOut;
            s_last_is_out = isOut;
            s_last_rc     = 0;
            return 0;
        }

        Result PlayTrack(const char* path) {
            /* Open file and allocate */
            auto source = OpenFile(path);
            R_UNLESS(source != nullptr, tune::FileOpenFailure);
            R_UNLESS(source->IsOpen(), tune::FileOpenFailure);
            R_UNLESS(source->SetupResampler(audoutGetChannelCount(), audoutGetSampleRate()), tune::VoiceInitFailure);

            AudioOutState state;
            R_TRY(audoutGetAudioOutState(&state));
            if (state == AudioOutState_Stopped) {
                R_TRY(audoutStartAudioOut());
            }

            g_source = source.get();

            // for the first buffer, use very small buffer sizes to reduce latency between songs.
            int first = 1;

            while (g_should_run && g_status == PlayerStatus::Playing) {
                if (g_should_pause) {
                    svcSleepThread(17'000'000);
                    continue;
                }

                AudioOutBuffer* buffer = NULL;
                for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
                    bool has_buffer = false;
                    R_TRY(audoutContainsAudioOutBuffer(&g_audout_buffer[i], &has_buffer));
                    if (!has_buffer) {
                        buffer = &g_audout_buffer[i];
                        break;
                    }
                }

                if (!buffer) {
                    u32 released_count;
                    R_TRY(audoutWaitPlayFinish(&buffer, &released_count, UINT64_MAX));
                }

                bool error = false;
                if (buffer) {
                    auto buffer_size = AUDIO_BUFFER_SIZE * sizeof(s16);
                    if (first) {
                        first--;
                        buffer_size = std::min(512 * sizeof(s16), buffer_size);
                    }

                    const auto nSamples = source->Resample((u8*)buffer->buffer, buffer_size);
                    if (nSamples <= 0) {
                        error = true;
                    } else {
                        buffer->data_size = nSamples;
                        R_TRY(audoutAppendAudioOutBuffer(buffer));
                    }
                }

                if (error || source->Done()) {
                    if (g_repeat != RepeatMode::One) {
                        Next();
                    }
                    break;
                }
            }

            g_source = nullptr;

            return 0;
        }

    }

    Result Initialize() {
        for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
            g_audout_buffer[i].buffer = AudioMemoryPool[i];
            g_audout_buffer[i].buffer_size = sizeof(AudioMemoryPool[i]);
        }

        R_TRY(audoutInitialize());
        SetVolume(config::get_volume());

        /* Fetch values from config, sanitize the return value */
        if (auto c = config::get_repeat(); c <= 2 && c >= 0) {
            SetRepeatMode(static_cast<RepeatMode>(c));
        }

        SetShuffleMode(static_cast<ShuffleMode>(config::get_shuffle()));
        SetDefaultTitleVolume(config::get_default_title_volume());

        // reserves memory so that we don't allocate later on.
        g_playlist.Init();

        /* --------------------------------------------------------------
         * pdmqry initialization — best effort, degrades gracefully.
         *
         * pdm:qry exposes only 3 sessions to user-land and libnx holds
         * one live. We only need query commands (which don't require an
         * active session), so clone the handle and close the original
         * to free a slot.
         *
         * If any step fails, g_pdmqry_available stays false and
         * PmdmntThreadFunc falls back to non-focus-aware behaviour.
         * -------------------------------------------------------------- */
        if (R_SUCCEEDED(pdmqryInitialize())) {
            Service* srv = pdmqryGetServiceSession();
            Service  clone;
            if (R_SUCCEEDED(serviceClone(srv, &clone))) {
                serviceClose(srv);
                std::memcpy(srv, &clone, sizeof(Service));
                g_pdmqry_available = true;
            } else {
                pdmqryExit();
            }
        }

        /* --------------------------------------------------------------
         * Startup-default pause.
         *
         * g_should_pause defaults to false, which means the very first
         * iteration of PlayTrack()'s decode loop will append a (small)
         * audio buffer to audout BEFORE PmdmntThreadFunc has had a chance
         * to consult the per-title "Auto-play Startup" config — producing
         * an audible blip whenever autoplay is meant to be off.
         *
         * Force-pause here. PmdmntThreadFunc's first poll will lift this
         * pause within ~10 ms iff the running title is allowed to play.
         * -------------------------------------------------------------- */
        g_should_pause = true;

        return 0;

    }

    void Exit() {
        if (g_pdmqry_available) {
            pdmqryExit();
            g_pdmqry_available = false;
        }
        g_should_run = false;
    }

    void TuneThreadFunc(void *) {
        {
            char load_path[PATH_SIZE_MAX];
            if (config::get_load_path(load_path, sizeof(load_path))) {
                // check if the path is a file or folder.
                FsDirEntryType type;
                if (R_SUCCEEDED(sdmc::GetType(load_path, &type))) {
                    if (type == FsDirEntryType_File) {
                        // path is a file, load single entry.
                        if (GetSourceType(load_path) != SourceType::NONE) {
                            Enqueue(load_path, std::strlen(load_path), EnqueueType::Back);
                        }
                    } else {
                        // path is a folder, load all entries sorted case-insensitively
                        // so the startup playlist order matches the browser's display order.
                        FsDir dir;
                        if (R_SUCCEEDED(sdmc::OpenDir(&dir, load_path, FsDirOpenMode_ReadFiles|FsDirOpenMode_NoFileSize))) {
                            // Collect all supported filenames first, then sort, then enqueue.
                            std::vector<std::string> file_names;
                            file_names.reserve(PLAYLIST_ENTRY_MAX);

                            std::vector<FsDirectoryEntry> entries(std::min(64, PLAYLIST_ENTRY_MAX));
                            s64 total;

                            while (R_SUCCEEDED(fsDirRead(&dir, &total, entries.size(), entries.data())) && total) {
                                for (s64 i = 0; i < total; i++) {
                                    if (GetSourceType(entries[i].name) != SourceType::NONE) {
                                        file_names.emplace_back(entries[i].name);
                                    }
                                }
                                if (static_cast<int>(file_names.size()) >= PLAYLIST_ENTRY_MAX)
                                    break;
                            }

                            fsDirClose(&dir);

                            // Sort case-insensitively — matches BrowserGui / addAllToPlaylist().
                            std::sort(file_names.begin(), file_names.end(), [](const std::string &a, const std::string &b) {
                                return strcasecmp(a.c_str(), b.c_str()) < 0;
                            });

                            char full_path[PATH_SIZE_MAX];
                            for (const auto &name : file_names) {
                                std::snprintf(full_path, sizeof(full_path), "%s/%s", load_path, name.c_str());
                                const Result rc = Enqueue(full_path, std::strlen(full_path), EnqueueType::Back);
                                if (rc == tune::OutOfMemory)
                                    break;
                            }
                        }
                    }
                }
            }
        }

        /* Run as long as we aren't stopped and no error has been encountered. */
        while (g_should_run) {
            g_current.Reset();
            {
                std::scoped_lock lk(g_mutex);

                const auto queue_size = g_playlist.Size();
                if (queue_size == 0) {
                    g_current.Reset();
                } else if (g_queue_position >= queue_size) {
                    g_queue_position = queue_size - 1;
                    continue;
                } else {
                    g_current = g_playlist.Get(g_queue_position, g_shuffle);
                }
            }

            /* Sleep if queue is empty. */
            if (!g_current.IsValid()) {
                svcSleepThread(100'000'000ul);
                continue;
            }

            g_status = PlayerStatus::Playing;
            /* Only play if playing and we have a track queued. */
            Result rc = PlayTrack(g_playlist.GetPath(g_current));

            /* Log error. */
            if (R_FAILED(rc)) {
                /* Remove track if something went wrong. */
                Remove(g_queue_position);
            }
        }

        audoutStopAudioOut();
        audoutExit();
    }

    void GpioThreadFunc(void *ptr) {
        GpioPadSession *session = static_cast<GpioPadSession *>(ptr);

        bool pre_unplug_pause = false;

        /* [0] Low == plugged in; [1] High == not plugged in. */
        GpioValue old_value = GpioValue_High;

        // TODO(TJ): pausing on headphone change should be a config option.
        while (g_should_run) {
            /* Fetch current gpio value. */
            GpioValue value;
            if (R_SUCCEEDED(gpioPadGetValue(session, &value))) {
                if (old_value == GpioValue_Low && value == GpioValue_High) {
                    pre_unplug_pause = g_should_pause;
                    g_should_pause     = true;
                } else if (old_value == GpioValue_High && value == GpioValue_Low) {
                    if (!pre_unplug_pause)
                        g_should_pause = false;
                }
                old_value = value;
            }
            svcSleepThread(10'000'000);
        }
    }

    void PmdmntThreadFunc(void *) {
        /* HOME-aware per-title pause/play policy.
         *
         * Two independent facts at every tick:
         *   (1) pmdmnt reports which process is foreground  (the "tid")
         *   (2) pdmqry reports whether that tid is in focus (focused?)
         *
         * A HOME press leaves the game process foreground but flips it
         * out of focus. We treat that as a SUSPENSION of the game's
         * playback state:
         *
         *   focused -> unfocused  : snapshot g_should_pause, apply
         *                           HOME's "Pause On Home" policy.
         *   unfocused -> focused  : restore the snapshotted state.
         *                           (A startup setting shouldn't re-fire
         *                            every time the user returns from HOME.)
         *   tid change (focused)  : genuine game switch — apply new
         *                           title's "Pause On Start" policy.
         *
         * IMPORTANT: pm::PollCurrentPidTid is EDGE-TRIGGERED — it only
         * returns true when the foreground PROCESS changes. Pressing
         * HOME during a game does not change the process (the game is
         * suspended, not closed), so PollCurrentPidTid stays silent.
         * We must therefore poll pdmqry EVERY tick to observe focus
         * flips.
         *
         * USER VETO: if the user has manually Paused, g_user_paused is
         * set and the policy engine cannot UNPAUSE the music until the
         * user Plays again. The policy engine CAN still pause (those
         * writes align with user intent anyway). This is implemented
         * by routing every policy write through policyWrite().
         *
         * First poll: whatever we observe, apply the title's startup
         * setting so the Initialize()-set pause is lifted (or not)
         * correctly. No audio reaches audout until this resolves.
         *
         * Per-title VOLUME is orthogonal — always applied on every tid
         * change, regardless of focus or user state.
         *
         * If pdmqry is unavailable we can't detect HOME, so we fall
         * back to the symmetric non-focus-aware behaviour (apply the
         * title's setting whenever the tid changes). */

        constexpr u64 kHomeScreenTid = 0x0100000000001000ULL;

        /* Tri-state policy for title transitions.
         *
         *   ForcePlay  — Play On Start is ON for this title
         *                (config::title_enabled(tid) = true)
         *   ForcePause — Pause On Start is ON for this title
         *                (config::title_pause_on_start(tid) = true)
         *   DoNothing  — neither key is set; keep whatever was playing
         *
         * HOME focus-loss uses the same resolver: "Pause On Home" in the
         * UI writes title_pause_on_start(kHomeScreenTid), so a configured
         * HOME always yields ForcePause; an unconfigured HOME yields
         * DoNothing (no surprise-pause on first launch). */
        enum class StartAction { DoNothing, ForcePlay, ForcePause };

        auto resolvePerTitlePolicy = [](u64 tid) -> StartAction {
            /* Pause On Start takes priority if somehow both keys are set
             * (mutual exclusion is enforced by the UI, but be defensive). */
            if (config::has_title_pause_on_start(tid) &&
                config::get_title_pause_on_start(tid))
                return StartAction::ForcePause;
            if (config::has_title_enabled(tid) &&
                config::get_title_enabled(tid))
                return StartAction::ForcePlay;
            return StartAction::DoNothing;
        };

        /* One-way veto: policy can pause freely but cannot unpause
         * while the user has explicitly paused. */
        auto policyWrite = [](bool should_pause) {
            if (!should_pause && g_user_paused)
                return;  // veto: user's Pause outranks this unpause
            g_should_pause = should_pause;
        };

        bool first_poll   = true;
        u64  current_tid  = 0;     // tracks the foreground tid across ticks
        u64  current_pid  = 0;     // tracks foreground pid for volume IPC
        bool last_focused = true;  // focus state observed last tick

        while (g_should_run) {
            /* ---- (1) Tid-change edge: update current_tid + volume ---- */
            {
                u64 new_pid{}, new_tid{};
                if (pm::PollCurrentPidTid(&new_pid, &new_tid)) {
                    current_pid = new_pid;

                    /* Per-title volume — always applied on tid change,
                     * regardless of user state (volume is separate from
                     * pause/play). */
                    g_title_volume = 1.f;
                    if (config::has_title_volume(new_tid)) {
                        g_use_title_volume = true;
                        SetTitleVolume(std::clamp(config::get_title_volume(new_tid), 0.f, VOLUME_MAX));
                    }

                    if (first_poll) {
                        /* Boot: use the global Auto-play Startup toggle.
                         * Per-title Play/Pause On Start are intentionally
                         * ignored at boot — the sysmodule may be spawned
                         * from any title or HOME, and the user's intent is
                         * captured by the single "should music play when
                         * the module starts?" question. */
                        policyWrite(!config::get_auto_play_startup());
                        current_tid = new_tid;
                        first_poll  = false;
                    } else {
                        /* Genuine title change.
                         *
                         * kHomeScreenTid is special: pmdmnt can report HOME
                         * as the foreground process while the game is still
                         * suspended in the background. HOME-related policy
                         * (Pause On Home) is applied exclusively by the
                         * focus-detection path below so we don't react to
                         * stale title_enabled(HOME) keys that old config
                         * files may contain, and don't double-fire policy.
                         *
                         * For every other title, apply Play/Pause On Start. */
                        if (new_tid != kHomeScreenTid) {
                            const auto action = resolvePerTitlePolicy(new_tid);
                            if (action == StartAction::ForcePlay) {
                                /* The user explicitly configured this title to
                                 * play — that IS their stated intent, so it
                                 * overrides any prior manual pause veto. */
                                g_user_paused = false;
                                policyWrite(false);
                            } else if (action == StartAction::ForcePause)
                                policyWrite(true);
                            /* DoNothing: leave g_should_pause as-is. */
                        }

                        current_tid  = new_tid;
                        last_focused = true;   // new foreground starts focused
                    }
                }
            }

            /* ---- (2) Level-triggered focus poll (every tick) ---- */
            if (!first_poll && g_pdmqry_available && current_tid != 0) {
                bool out = false;
                const Result rc = isApplicationOutOfFocus(current_tid, &out);
                if (R_SUCCEEDED(rc)) {
                    const bool new_focused = !out;
                    if (new_focused != last_focused) {
                        if (!new_focused) {
                            /* Pressed HOME while in a game.
                             * Snapshot current state, then apply HOME's
                             * Pause On Home policy (if configured).
                             * DoNothing → keep playing through the HOME visit. */
                            g_saved_pause_state = g_should_pause;
                            const auto action = resolvePerTitlePolicy(kHomeScreenTid);
                            if (action == StartAction::ForcePause)
                                policyWrite(true);
                            else if (action == StartAction::ForcePlay)
                                policyWrite(false);
                            /* DoNothing: leave playback state unchanged. */
                        } else {
                            /* Returned to the same game from HOME.
                             *
                             * Re-apply Play On Start / Pause On Start so that
                             * coming back from HOME behaves the same as
                             * entering the title fresh:
                             *
                             *   ForcePlay  → start playing (clears user-pause
                             *                veto just like a title launch)
                             *   ForcePause → pause
                             *   DoNothing  → restore the state captured at
                             *                focus-loss (continue whatever
                             *                was happening before HOME) */
                            const auto action = resolvePerTitlePolicy(current_tid);
                            if (action == StartAction::ForcePlay) {
                                g_user_paused = false;
                                policyWrite(false);
                            } else if (action == StartAction::ForcePause) {
                                policyWrite(true);
                            } else {
                                policyWrite(g_saved_pause_state);
                            }
                        }
                        last_focused = new_focused;
                    }
                }
                /* else: current_tid isn't a tracked retail app (HOME
                 * itself, applet, etc.) — leave focus state untouched. */
            }

            /* ---- (3) Continuously apply per-title master volume ---- */
            // sadly, we can't simply apply auda when the title changes
            // as it seems to apply to quickly, before the title opens audio
            // services, so the changes don't apply.
            // best option is to repeatdly set the out :/
            if (current_pid) {
                const auto v = g_use_title_volume ? g_title_volume : g_default_title_volume;
                audWrapperSetProcessMasterVolume(current_pid, 0, v);
                // audWrapperSetProcessRecordVolume(current_pid, 0, v);
            }

            svcSleepThread(10'000'000);
        }
    }

    bool GetStatus() {
        return !g_should_pause;
    }

    void Play() {
        g_should_pause       = false;
        g_user_paused        = false;  // clears the pause-veto
        g_saved_pause_state  = false;  // keep focus-suspend snapshot in sync
    }

    void Pause() {
        g_should_pause       = true;
        g_user_paused        = true;   // one-way veto: policy cannot unpause us
        g_saved_pause_state  = true;   // keep focus-suspend snapshot in sync
    }

    void Next() {
        bool pause = false;
        {
            std::scoped_lock lk(g_mutex);

            if (g_queue_position < g_playlist.Size() - 1) {
                g_queue_position++;
            } else {
                g_queue_position = 0;
                if (g_repeat == RepeatMode::Off)
                    pause = true;
            }
        }
        g_status             = PlayerStatus::FetchNext;
        g_should_pause       = pause;
        g_user_paused        = pause;  // honour end-of-queue stop as an intentional pause
        g_saved_pause_state  = pause;
    }

    void Prev() {
        {
            std::scoped_lock lk(g_mutex);

            if (g_queue_position > 0) {
                g_queue_position--;
            } else {
                g_queue_position = g_playlist.Size() - 1;
            }
        }
        g_status             = PlayerStatus::FetchNext;
        g_should_pause       = false;
        g_user_paused        = false;
        g_saved_pause_state  = false;
    }

    float GetVolume() {
        float volume = 1.F;
        audoutGetAudioOutVolume(&volume);
        return volume;
    }

    void SetVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        audoutSetAudioOutVolume(volume);
        config::set_volume(volume);
    }

    float GetTitleVolume() {
        return g_title_volume;
    }

    void SetTitleVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        g_title_volume = volume;
        g_use_title_volume = true;
    }

    float GetDefaultTitleVolume() {
        return g_default_title_volume;
    }

    void SetDefaultTitleVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        g_default_title_volume = volume;
        config::set_default_title_volume(volume);
    }

    RepeatMode GetRepeatMode() {
        return g_repeat;
    }

    void SetRepeatMode(RepeatMode mode) {
        g_repeat = mode;
    }

    ShuffleMode GetShuffleMode() {
        return g_shuffle;
    }

    void SetShuffleMode(ShuffleMode mode) {
        std::scoped_lock lk(g_mutex);
    
        if (g_shuffle == ShuffleMode::Off && mode == ShuffleMode::On) {
            // Shuffle the playlist randomly first.
            g_playlist.Shuffle();
    
            // Then pin the currently playing track to position 0 in the
            // shuffle list so it stays playing without any interruption,
            // and next/prev navigate forward from there.
            if (g_current.IsValid()) {
                const u32 cur_pos = g_playlist.GetIndexFromID(g_current, ShuffleMode::On);
                if (cur_pos != 0)
                    g_playlist.Swap(0, cur_pos, ShuffleMode::On);
                g_queue_position = 0;
            }
    
        } else if (g_shuffle == ShuffleMode::On && mode == ShuffleMode::Off) {
            // Restore queue_position to wherever the current track sits in
            // the original unshuffled list, so next/prev work naturally
            // from that position in the original order.
            if (g_current.IsValid())
                g_queue_position = g_playlist.GetIndexFromID(g_current, ShuffleMode::Off);
        }
    
        g_shuffle = mode;
    }

    u32 GetPlaylistSize() {
        std::scoped_lock lk(g_mutex);

        return g_playlist.Size();
    }

    Result GetPlaylistItem(u32 index, char *buffer, size_t buffer_size) {
        std::scoped_lock lk(g_mutex);

        const auto path = g_playlist.GetPath(index, g_shuffle);
        R_UNLESS(path, tune::OutOfRange);

        std::snprintf(buffer, buffer_size, "%s", path);

        return 0;
    }

    Result GetCurrentQueueItem(CurrentStats *out, char *buffer, size_t buffer_size) {
        R_UNLESS(g_source != nullptr, tune::NotPlaying);
        R_UNLESS(g_source->IsOpen(), tune::NotPlaying);

        {
            std::scoped_lock lk(g_mutex);

            const auto path = g_playlist.GetPath(g_current);
            R_UNLESS(path, tune::NotPlaying);

            std::snprintf(buffer, buffer_size, "%s", path);
        }

        auto [current, total] = g_source->Tell();
        int sample_rate       = g_source->GetSampleRate();

        out->sample_rate   = sample_rate;
        out->current_frame = current;
        out->total_frames  = total;

        return 0;
    }

    void ClearQueue() {
        {
            std::scoped_lock lk(g_mutex);

            g_playlist.Clear();
            g_queue_position = 0;
        }
        g_status = PlayerStatus::FetchNext;
    }

    // currently unused (and untested).
    void MoveQueueItem(u32 src, u32 dst) {
        std::scoped_lock lk(g_mutex);

        if (!g_playlist.Swap(src, dst, g_shuffle)) {
            return;
        }

        if (g_queue_position == src) {
            g_queue_position = dst;
        }
    }

    void Select(u32 index) {
        {
            std::scoped_lock lk(g_mutex);

            const auto size = g_playlist.Size();
            if (!size) {
                return;
            }

            g_queue_position = std::min(index, size - 1);
        }
        g_status             = PlayerStatus::FetchNext;
        g_should_pause       = false;
        g_user_paused        = false;
        g_saved_pause_state  = false;
    }

    void Seek(u32 position) {
        if (g_source != nullptr && g_source->IsOpen())
            g_source->Seek(position);
    }

    Result Enqueue(const char *buffer, size_t buffer_length, EnqueueType type) {
        if (GetSourceType(buffer) == SourceType::NONE)
            return tune::InvalidPath;

        /* Ensure file exists. */
        if (!sdmc::FileExists(buffer))
            return tune::InvalidPath;

        std::scoped_lock lk(g_mutex);

        if (!g_playlist.Add(buffer, type)) {
            return tune::OutOfMemory;
        }

        // check if the current position still points to the same entry, update if not.
        if (g_current.IsValid() && g_current.id != g_playlist.Get(g_queue_position, g_shuffle).id) {
            g_queue_position = g_playlist.GetIndexFromID(g_current, g_shuffle);
            g_current = g_playlist.Get(g_queue_position, g_shuffle);
        }

        return 0;
    }

    Result Remove(u32 index) {
        std::scoped_lock lk(g_mutex);

        /* Ensure we don't operate out of bounds. */
        R_UNLESS(g_playlist.Size(), tune::QueueEmpty);

        if (!g_playlist.Remove(index, g_shuffle)) {
            return tune::OutOfRange;
        }

        /* Fetch a new track if we deleted the current song. */
        const bool fetch_new = g_queue_position == index;

        /* Lower current position if needed. */
        if (g_queue_position > index) {
            g_queue_position--;
        }

        if (fetch_new)
            g_status = PlayerStatus::FetchNext;

        return 0;
    }

}