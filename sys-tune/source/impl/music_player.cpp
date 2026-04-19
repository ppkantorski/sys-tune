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

        /* Title-transition fade parameters.
         * 50 steps × 20 ms = 1 000 ms total ramp time. */
        constexpr int  kFadeSteps           = 200;
        constexpr u64  kFadeStepIntervalNs  = 5'000'000ULL;

        AudioOutBuffer g_audout_buffer[AUDIO_BUFFER_COUNT];
        alignas(0x1000) s16 AudioMemoryPool[AUDIO_BUFFER_COUNT][(AUDIO_BUFFER_SIZE + 0xFFF) & ~0xFFF];
        static_assert((sizeof(AudioMemoryPool[0]) % 0x2000) == 0, "Audio Memory pool needs to be page aligned!");

        bool g_should_pause      = false;
        bool g_should_run        = true;

        /* Event-based wake-ups — replace sleep-polling in two hot paths:
         *
         *   g_unpause_event       — signaled whenever g_should_pause may have
         *                           become false, g_status changed to FetchNext,
         *                           or the sysmodule is shutting down.  Allows
         *                           PlayTrack's pause loop to block indefinitely
         *                           instead of waking every 50 ms.
         *
         *   g_queue_changed_event — signaled by Enqueue() when a track is added.
         *                           Allows TuneThreadFunc's empty-queue loop to
         *                           block indefinitely instead of waking every
         *                           100 ms.
         *
         * Both are auto-clear (one-shot): the signal is consumed on the first
         * waiter wake-up, preventing stale signals from stacking up. */
        LEvent g_unpause_event;
        LEvent g_queue_changed_event;

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
                    // Block until Play/Next/Select/policy signals us, or 500 ms safety timeout.
                    // leventClear resets the signal state so the next wait actually blocks
                    // rather than returning immediately (LEvent is manual-clear by default).
                    leventWait(&g_unpause_event, 500'000'000ULL);
                    leventClear(&g_unpause_event);
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
         * Set g_should_pause directly from the Auto-play Startup config
         * before any thread starts.  This guarantees the correct initial
         * state regardless of whether PmdmntThreadFunc's PollCurrentPidTid
         * edge ever fires (it won't if no foreground process change occurs
         * after the sysmodule spawns — e.g. the user stays on HOME the
         * whole time).  config:: reads are safe here because sdmc is open
         * before Initialize() is called (evidenced by get_volume() working
         * in the same function).
         * -------------------------------------------------------------- */
        g_should_pause = !config::get_auto_play_startup();

        return 0;

    }

    void Exit() {
        if (g_pdmqry_available) {
            pdmqryExit();
            g_pdmqry_available = false;
        }
        g_should_run = false;
        leventSignal(&g_unpause_event);       // unblock PlayTrack's pause wait
        leventSignal(&g_queue_changed_event); // unblock TuneThreadFunc's empty-queue wait
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

            /* Block until Enqueue() adds something — no reason to wake
             * periodically when the queue is genuinely empty. */
            if (!g_current.IsValid()) {
                leventWait(&g_queue_changed_event, 500'000'000ULL);
                leventClear(&g_queue_changed_event);
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

        /* Read the actual headphone-jack state before entering the loop.
         * Without this, old_value defaults to High ("unplugged"), so if
         * headphones are already plugged in at sysmodule start the first
         * gpioPadGetValue() call sees a High→Low transition and forces
         * g_should_pause = false, defeating Auto-play Startup OFF. */
        GpioValue old_value = GpioValue_High;
        gpioPadGetValue(session, &old_value);

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
            // 100 ms is imperceptibly fast for headphone-jack detection and cuts
            // wakeups from 100/sec to 10/sec compared to the old 10 ms poll.
            svcSleepThread(100'000'000);
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
            /* The HOME screen tid always uses its own per-title entry
             * (the dedicated "Pause On Home" toggle).  Never apply the
             * global Play/Pause On Title defaults to it. */
            constexpr u64 kHomeScreenTidLocal = 0x0100000000001000ULL;

            if (tid != kHomeScreenTidLocal && config::get_default_on_start(tid)) {
                /* This title defers to the global defaults. */
                if (config::get_pause_on_title())
                    return StartAction::ForcePause;
                if (config::get_play_on_title())
                    return StartAction::ForcePlay;
                return StartAction::DoNothing;
            }

            /* Per-title override path (Default On Start is OFF,
             * or this is the HOME screen tid). */
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
            if (!should_pause)
                leventSignal(&g_unpause_event); // wake PlayTrack's pause wait
        };

        /* ---------------------------------------------------------------
         * Non-blocking fade state machine.
         *
         * The old blocking fadeOut/fadeIn lambdas owned PmdmntThreadFunc's
         * thread for up to 1 s per call.  During that time section (2)'s
         * pdmqry poll could never run, so a HOME press mid-fade was
         * invisible until the fade completed — leaving music stuck paused.
         *
         * The new design advances the fade by ONE STEP per main-loop tick
         * at the bottom of the loop.  Section (2) runs every 5 ms during
         * any active fade and can redirect it (e.g. FadeOut → FadeIn) on
         * the very tick pdmqry posts an OutOfFocus event.
         *
         * FadeDir::Out  — volume decreases each tick (fade_step counts down)
         * FadeDir::In   — volume increases each tick (fade_step counts up)
         *
         * fade_pause: when true, call policyWrite(true) and start a short
         *             drain countdown when FadeOut reaches silence.
         * fade_drain: ticks remaining for audio-buffer drain after pause;
         *             volume stays at 0 until the counter expires, then
         *             restores to fade_vol to avoid a pop on next play.
         * --------------------------------------------------------------- */
        enum class FadeDir { None, Out, In };
        FadeDir fade_dir   = FadeDir::None;
        int     fade_step  = 0;     // current step (Out: counts down; In: counts up)
        int     fade_total = 0;     // total steps
        float   fade_vol   = 0.f;   // master volume captured at fade start
        bool    fade_pause = false; // call policyWrite(true) when FadeOut ends
        int     fade_drain = 0;     // ticks to hold silence after fadeOut+pause

        /* Silent-hold phase (first-launch ForcePause only).
         *
         * After a first-launch FadeOut reaches silence, we do NOT
         * immediately call policyWrite(true).  Instead we enter a hold
         * for fade_hold_target ticks (volume stays 0, music still plays).
         * Section (2) keeps running at 5 ms; if pdmqry delivers
         * OutOfFocus during the hold, startFadeIn cancels it and ramps
         * back up — no audible pause.  Only when the hold expires do we
         * call policyWrite(true) and start the normal drain.
         *
         * This extends total coverage from (stability_window + fade) to
         * (stability_window + fade + hold), giving pdmqry's slow NAND-
         * backed OutOfFocus write enough time to arrive. */
        int     fade_hold        = 0;   // remaining hold ticks
        int     fade_hold_target = 0;   // hold ticks to apply when FadeOut completes

        /* Stability window for first-launch ForcePause.
         *
         * Problem: pmdmnt fires and pdmqry logs InFocus as soon as the game
         * registers with appletmgr (~100-500 ms after launch).  But when HOME
         * is pressed during loading, pdmqry logs OutOfFocus with 1-2 s of
         * additional latency (NAND write pressure during game loading).  If we
         * call startFadeOut the moment InFocus arrives, the full 1 s fade runs
         * to completion before OutOfFocus ever appears — leaving music stuck
         * paused with no reversal.
         *
         * Solution: on the FIRST InFocus for a new TID with action=ForcePause,
         * instead of fading immediately, run a 1000 ms polling window at 5 ms
         * ticks.  Section (2) keeps running every tick.  If OutOfFocus arrives
         * within the window, we know the user pressed HOME during loading —
         * skip the fade and apply HOME policy.  If the window expires, the game
         * is genuinely in focus and we start the fade (which remains
         * interruptible by section (2) throughout its duration). */
        int         fade_pending_ticks  = 0;
        StartAction fade_pending_action = StartAction::DoNothing;

        auto fadeActive = [&]() -> bool {
            return fade_dir != FadeDir::None || fade_drain > 0 || fade_pending_ticks > 0 || fade_hold > 0;
        };

        /* startFadeOut: begin (or redirect to) a non-blocking fade-out.
         *
         *   - If a FadeIn is in progress, reverse it from the current
         *     position (proportionally mapped to `steps`).
         *   - If already paused, call policyWrite(true) and return.
         *   - Otherwise capture the current volume and start the ramp. */
        auto startFadeOut = [&](int steps, bool do_pause, int hold_ticks = 0) {
            if (fade_dir == FadeDir::In) {
                /* Reverse FadeIn → FadeOut from current position. */
                int out_step   = int(float(fade_step) / float(fade_total) * float(steps));
                fade_dir         = FadeDir::Out;
                fade_step        = out_step;
                fade_total       = steps;
                fade_pause       = do_pause;
                fade_drain       = 0;
                fade_hold_target = do_pause ? hold_ticks : 0;
                return;
            }
            if (fade_hold > 0) {
                /* Already in the silent-hold phase (volume=0, still playing).
                 * There is nothing left to fade out — just commit the pause. */
                fade_hold        = 0;
                fade_hold_target = 0;
                if (do_pause) {
                    policyWrite(true);
                    fade_drain = (3 * AUDIO_LATENCY_MS + 4) / 5;
                }
                return;
            }
            if (fade_drain > 0) {
                /* Cancel drain and restore volume before starting new fade. */
                fade_drain = 0;
                audoutSetAudioOutVolume(fade_vol);
            }
            if (g_should_pause) {
                if (do_pause) policyWrite(true);
                return;
            }
            fade_vol         = GetVolume();
            fade_dir         = FadeDir::Out;
            fade_step        = steps - 1;
            fade_total       = steps;
            fade_pause       = do_pause;
            fade_drain       = 0;
            fade_hold_target = do_pause ? hold_ticks : 0;
        };

        /* startFadeIn: begin (or redirect to) a non-blocking fade-in.
         *
         *   - If a FadeOut is in progress, REVERSE it from the current
         *     position (proportionally mapped to `steps`).  This is the
         *     key path for HOME-press interrupting a title-launch fade:
         *     no blocking, no hidsys, purely driven by section (2)'s
         *     pdmqry poll now running every 5 ms.
         *   - If in a drain countdown, cancel it and fade in from silence.
         *   - If music is already playing, ensure policyWrite(false). */
        auto startFadeIn = [&](int steps) {
            if (fade_dir == FadeDir::Out) {
                /* Reverse FadeOut → FadeIn from current position.
                 * g_should_pause is still false (FadeOut never set it),
                 * so no policyWrite needed — just flip direction. */
                int rev_step = int(float(fade_step) / float(fade_total) * float(steps));
                fade_dir   = FadeDir::In;
                fade_step  = rev_step;
                fade_total = steps;
                fade_pause = false;
                fade_drain = 0;
                return;
            }
            if (fade_hold > 0) {
                /* Silent-hold phase: volume is already 0, music is still
                 * playing (g_should_pause=false).  Cancel the hold and
                 * ramp straight back up — no policyWrite needed. */
                fade_hold        = 0;
                fade_hold_target = 0;
                fade_dir   = FadeDir::In;
                fade_step  = 1;
                fade_total = steps;
                fade_pause = false;
                fade_drain = 0;
                return;
            }
            if (fade_drain > 0) {
                /* Was draining after a FadeOut+pause.
                 *
                 * CRITICAL: do NOT call GetVolume() here.  audout was left
                 * at 0.f when the FadeOut completed (the last advance step
                 * wrote fade_vol * 0/total = 0), so GetVolume() would
                 * return 0 and the subsequent FadeIn would silently ramp
                 * 0 * (step/total) = 0 every tick — no audible fade-in.
                 *
                 * fade_vol was captured by startFadeOut before the ramp
                 * began and has not changed; it is already the correct
                 * pre-fade master volume.  Start the FadeIn directly from
                 * the current silence (audout already at 0). */
                fade_drain = 0;
                fade_dir   = FadeDir::In;
                fade_step  = 1;
                fade_total = steps;
                fade_pause = false;
                /* g_should_pause=true was set by policyWrite() at the end
                 * of the FadeOut; un-pause so audio can flow again. */
                policyWrite(false);
                return;
            }
            if (!g_should_pause) {
                policyWrite(false);
                return;
            }
            /* Standard path: paused but drain has already expired.
             * audoutSetAudioOutVolume(fade_vol) ran when drain hit 0,
             * so GetVolume() correctly returns the pre-fade level here. */
            fade_vol   = GetVolume();
            fade_dir   = FadeDir::In;
            fade_step  = 1;
            fade_total = steps;
            fade_pause = false;
            fade_drain = 0;
            audoutSetAudioOutVolume(0.f);
            policyWrite(false);
        };

        bool first_poll   = true;
        u64  current_tid  = 0;
        u64  current_pid     = 0;
        bool last_focused    = true;
        bool is_first_focus  = true;  // true after TID change; false after first InFocus fires
        bool s_vol_needs_reapply  = false; // set on focus-loss AND focus-return; triggers vol write
        bool s_in_vol_transition  = false; // true during tight-poll window after focus flip; drives 1 ms sleep
        // Hoisted from inside if(current_pid) so the adaptive sleep expression
        // at the bottom of the loop can read them without scope issues.
        int  s_retry_ticks      = 0;
        int  s_transition_ticks = 0;

        /* Immediately write sys-tune's per-title master volume to the
         * foreground game process.  Called at the TOP of each focus
         * transition branch, BEFORE any blocking fadeIn/fadeOut, so the
         * game audio correction and the music fade start simultaneously.
         * Section (3)'s s_transition_ticks window then keeps correcting
         * for any post-resume system audproc reset.
         * Must be defined AFTER current_pid so the [&] capture is valid. */
        auto applyTitleVolNow = [&]() {
            if (!current_pid) return;
            const auto v = g_use_title_volume ? g_title_volume : g_default_title_volume;
            if (R_SUCCEEDED(audWrapperInitialize())) {
                audWrapperSetProcessMasterVolume(current_pid, 0, v);
                audWrapperExit();
            }
        };

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
                        /* Boot: g_should_pause was already committed in
                         * Initialize() from get_auto_play_startup().
                         * Do NOT call policyWrite() here — that would race
                         * with Initialize() and unnecessarily re-reads
                         * the config.  Just record the initial tid so the
                         * else-branch fires on the next genuine title
                         * change.  Per-title Play/Pause On Start are
                         * intentionally skipped at boot. */
                        current_tid = new_tid;

                        /* Seed last_focused from the actual observed focus
                         * state, NOT the hard-coded `true` default.  Without
                         * this, starting the sysmodule while the user is on
                         * HOME with a game suspended in the background (game
                         * is OOF) makes the very next focus-detection tick
                         * see a fake `true → false` transition and fire the
                         * HOME-press handler — which then applies HOME's
                         * policy (e.g. `fadeIn()` if "Play On Home" was ever
                         * toggled on, or `fadeOut()` if "Pause On Home" is
                         * on) and silently overrides the Auto-play Startup
                         * setting Initialize() just committed.
                         *
                         * The user never pressed HOME — they were already on
                         * HOME when the sysmodule spawned.  Seeding from the
                         * real state means the first focus tick sees
                         * `new_focused == last_focused` and no handler fires,
                         * so Auto-play Startup is preserved.  For
                         * kHomeScreenTid and non-application tids,
                         * isApplicationOutOfFocus returns non-zero and we
                         * leave last_focused at its default — focus detection
                         * skips those tids anyway, so it doesn't matter. */
                        if (g_pdmqry_available && new_tid != kHomeScreenTid && new_tid != 0) {
                            bool initial_out = false;
                            if (R_SUCCEEDED(isApplicationOutOfFocus(new_tid, &initial_out))) {
                                last_focused = !initial_out;
                            }
                        }

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
                            /* Register the new TID.  Do NOT apply any policy
                             * here.  pmdmnt fires the instant PM creates the
                             * game process, which is BEFORE the game connects
                             * its applet session to appletmgr.  If the user
                             * presses HOME in that window, pdmqry will have
                             * zero events for this TID — so we cannot tell
                             * whether the user is actually in the game yet.
                             *
                             * Deferring to section (2) handles all three cases
                             * cleanly:
                             *
                             *   (a) pdmqry logs InFocus  → section (2) fires,
                             *       applies title policy (ForcePause → fadeOut).
                             *       The fade is interruptible: if HOME is pressed
                             *       after registration, pdmqry logs OutOfFocus
                             *       and the fade loop catches it.
                             *
                             *   (b) pdmqry logs OutOfFocus only (user pressed
                             *       HOME after partial registration) → section (2)
                             *       sees new_focused=false vs last_focused=false
                             *       → no transition fires → music keeps playing.
                             *       (section (2) only fires on edge changes.)
                             *
                             *   (c) pdmqry logs nothing at all (game suspended
                             *       during loading before appletmgr handshake)
                             *       → section (2) never fires → last_focused
                             *       stays false → music keeps playing. ✓
                             *
                             * Cases (b) and (c) are the user-reported bug.
                             * Section (0)'s home-button event handles them:
                             * if InFocus arrives and a FadeOut starts, HOME
                             * reverses it instantly via the hid:sys event.
                             * If HOME fires before InFocus, last_focused is
                             * still false so section (0) skips — no fade. */
                            current_tid   = new_tid;
                            last_focused  = false;   // neutral; section (2) will set true on InFocus
                            is_first_focus = true;   // so section (2) uses full fade on first InFocus
                            fade_pending_ticks  = 0; // cancel any pending fade from prior title
                            fade_pending_action = StartAction::DoNothing;
                            fade_hold        = 0;    // cancel any hold from prior title
                            fade_hold_target = 0;
                        } else {
                            /* HOME became the foreground process (game exited).
                             *
                             * Two responsibilities:
                             *
                             * (A) ForcePlay/ForcePause fallback — if pdmqry never
                             *     logged an OutOfFocus event for the game (race:
                             *     game launched and HOME'd before appletmgr
                             *     registered its applet session), last_focused
                             *     is still true and section (2) never fired.
                             *     Apply HOME policy now as guaranteed fallback.
                             *     Guarded by last_focused so we don't double-fire
                             *     when section (2) already handled the transition.
                             *
                             * (B) Stale-TID reset — unconditionally zero
                             *     current_tid so section (2) stops querying the
                             *     now-exited game's TID.  Without this, pdmqry's
                             *     exit-sequence events (the game can briefly
                             *     appear InFocus during teardown / close animation)
                             *     look like a HOME-return transition: section (2)
                             *     sees new_focused=true with last_focused=false
                             *     (just set by this handler) and fires the
                             *     HOME-return ForcePause — producing the audible
                             *     "fade in for a second, then fade back out" the
                             *     user observes when exiting a game. */
                            if (!first_poll && current_tid != 0
                                    && current_tid != kHomeScreenTid) {
                                if (last_focused) {
                                    /* (A) pdmqry missed the HOME press — apply HOME
                                     * policy now, same as section (2)'s !new_focused
                                     * branch would have done. */
                                    g_saved_pause_state = g_should_pause;
                                    const auto action = resolvePerTitlePolicy(kHomeScreenTid);
                                    if (action == StartAction::ForcePause) {
                                        if (fade_dir == FadeDir::Out) {
                                            fade_pause       = true;
                                            fade_hold_target = 0; // no hold on game exit
                                        } else {
                                            startFadeOut(kFadeSteps / 2, /*do_pause=*/true);
                                        }
                                    } else if (action == StartAction::ForcePlay) {
                                        g_user_paused = false;
                                        startFadeIn(kFadeSteps / 2);
                                    } else {
                                        /* DoNothing: keep music playing as-is.
                                         * Reverse any in-progress title-launch FadeOut or
                                         * silent hold — the music should not be paused when
                                         * the user goes back to HOME. */
                                        if (fade_dir == FadeDir::Out || fade_hold > 0) {
                                            startFadeIn(kFadeSteps / 2);
                                        }
                                    }
                                }
                                /* (B) Game has exited — clear the stale TID so
                                 * section (2) skips on this and all future ticks
                                 * until a new game is launched. */
                                current_tid    = 0;
                                last_focused   = true;  // reset for next launch
                                is_first_focus = true;  // next game gets full fade
                                fade_pending_ticks  = 0; // game exited: cancel any pending fade
                                fade_pending_action = StartAction::DoNothing;
                                fade_hold        = 0;   // cancel any hold
                                fade_hold_target = 0;
                            }
                        }
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
                             *
                             * Do NOT call applyTitleVolNow() here.  Two reasons:
                             *
                             *   (1) UltraGB calls gb_game_vol_suppress() at the
                             *       same moment, which intentionally restores the
                             *       game audproc to 1.0 before the game sleeps.
                             *       Writing our per-title level on top of that
                             *       races with — and undoes — that suppress,
                             *       making the game audio briefly audible at the
                             *       sys-tune level during the HOME transition.
                             *
                             *   (2) The Switch resets audproc during the RESUME
                             *       sequence (not the suspend), so any write here
                             *       gets overwritten before it can help anyway.
                             *       The HOME-return branch below is the right
                             *       place to apply the correction.
                             *
                             * Snapshot state and apply HOME's pause policy.
                             *
                             * KEY: if a title-launch FadeOut is currently in
                             * progress (started by the new_focused branch below),
                             * startFadeIn will REVERSE it from the current volume
                             * position — no blocking, no hidsys.  The fade was
                             * non-blocking so section (2) ran freely, caught the
                             * OutOfFocus event, and is redirecting the ramp here. */
                            g_saved_pause_state = g_should_pause;
                            /* Cancel any pending stability window — user is on
                             * HOME regardless of what the window was waiting for. */
                            fade_pending_ticks  = 0;
                            fade_pending_action = StartAction::DoNothing;
                            const auto action = resolvePerTitlePolicy(kHomeScreenTid);
                            if (action == StartAction::ForcePause) {
                                if (fade_dir == FadeDir::Out) {
                                    /* Already fading out — just ensure it pauses
                                     * at the end (it may have been a no-pause
                                     * fade from a previous edge case). */
                                    fade_pause       = true;
                                    fade_hold_target = 0; // HOME exit: no hold, pause immediately
                                } else {
                                    startFadeOut(kFadeSteps / 2, /*do_pause=*/true);
                                }
                            } else if (action == StartAction::ForcePlay) {
                                /* Home Focus = Play: reverse any title-launch fade or
                                 * silent hold.  startFadeIn handles all cases:
                                 * FadeOut in progress → reverse it; hold active →
                                 * cancel and fade in; already paused → unpause. */
                                g_user_paused = false;
                                startFadeIn(kFadeSteps / 2);
                            } else {
                                /* DoNothing: keep music playing as-is.
                                 * Reverse any in-progress title-launch FadeOut or
                                 * silent hold via startFadeIn. */
                                if (fade_dir == FadeDir::Out || fade_hold > 0) {
                                    startFadeIn(kFadeSteps / 2);
                                }
                            }
                        } else {
                            /* Returned to game from HOME, OR first InFocus
                             * detected after a fresh game launch.
                             *
                             * is_first_focus distinguishes the two cases so
                             * we can use a full 1 s fade on first launch and
                             * a faster 0.5 s fade on HOME returns. */
                            {
                                const auto v_now = g_use_title_volume
                                    ? g_title_volume : g_default_title_volume;
                                if (std::fabs(v_now - 1.0f) > 0.01f) {
                                    applyTitleVolNow();
                                    s_vol_needs_reapply = true;
                                }
                            }

                            const bool was_first_focus = is_first_focus;
                            const int fade_steps = is_first_focus
                                ? kFadeSteps         // first launch: full 1 s fade
                                : kFadeSteps / 2;    // HOME return: faster 0.5 s fade
                            is_first_focus = false;

                            const auto action = resolvePerTitlePolicy(current_tid);
                            if (action == StartAction::ForcePlay) {
                                g_user_paused = false;
                                startFadeIn(fade_steps);
                            } else if (action == StartAction::ForcePause) {
                                if (was_first_focus) {
                                    /* FIRST InFocus for this title with ForcePause.
                                     *
                                     * Start the fade immediately (no stability window)
                                     * with a 1500 ms silent hold after it (hold_ticks=300).
                                     * After the 1 s fade reaches silence, policyWrite(true)
                                     * is deferred: volume stays 0, music still playing.
                                     * kHomeScreenTid fires the instant HOME is pressed via
                                     * pmdmnt — no pdmqry latency involved — so it handles
                                     * HOME reliably at any point during the fade or hold.
                                     * The hold just provides extra coverage for the rare case
                                     * where pmdmnt fires kHomeScreenTid after the fade is
                                     * already complete (e.g. very slow game loading). */
                                    startFadeOut(fade_steps, /*do_pause=*/true, /*hold_ticks=*/600);
                                } else {
                                    /* HOME return → ForcePause: start immediately
                                     * (game is already fully loaded, no hold needed). */
                                    startFadeOut(fade_steps, /*do_pause=*/true);
                                }
                            } else {
                                // Restore snapshotted state; fade in if resuming play.
                                if (!g_saved_pause_state && g_should_pause)
                                    startFadeIn(fade_steps);
                                else
                                    policyWrite(g_saved_pause_state);
                            }
                        }
                        last_focused = new_focused;
                    }
                }
                /* else: current_tid isn't a tracked retail app (HOME
                 * itself, applet, etc.) — leave focus state untouched. */
            }

            /* ---- (3) Apply per-title master volume on legitimate events only ----
             *
             * Three write triggers:
             *
             *   (a) PID change — game audio process may not be registered yet,
             *       so retry on failure up to 300 ms.  Stop on first success.
             *
             *   (b) g_title_volume changed — user moved the slider.  One write.
             *
             *   (c) Focus transition (HOME press and HOME return) — uses a
             *       dedicated s_transition_ticks window that keeps writing every
             *       tick for 300 ms EVEN AFTER a successful write.  This is
             *       necessary because the Switch resets the process's audproc
             *       volume during its resume sequence, AFTER the focus-return
             *       event is posted to pdmqry and AFTER our first write lands.
             *       A single write therefore gets silently overwritten by the
             *       system, producing the brief full-volume flash.  Writing
             *       every 10 ms for 300 ms means any system reset is corrected
             *       within one tick — imperceptible to the user.
             *       Outside this window sys-tune is event-driven and never
             *       writes in steady state, so UltraGB cooperation is intact.
             */
            if (current_pid) {
                const auto v = g_use_title_volume ? g_title_volume : g_default_title_volume;
                static u64   s_last_vol_pid     = 0;
                static float s_last_vol         = -1.f;
                // s_retry_ticks and s_transition_ticks declared at function scope above.
                static bool  s_applied_ok       = false;

                if (current_pid != s_last_vol_pid) {
                    s_last_vol_pid     = current_pid;
                    s_last_vol         = -1.f;
                    s_retry_ticks      = 30;
                    s_applied_ok       = false;
                    s_transition_ticks = 0;
                }

                if (s_vol_needs_reapply) {
                    s_applied_ok        = false;
                    s_retry_ticks       = 30;
                    s_last_vol          = -1.f;
                    s_transition_ticks  = 50;   // 50 × 1 ms = 50 ms tight-poll window; the
                                                // per-tick sleep shrinks to 1 ms while this
                                                // is non-zero so any post-resume audproc reset
                                                // is corrected within 1 ms, not up to 10 ms.
                    s_vol_needs_reapply = false;
                }

                const bool value_changed = (v != s_last_vol);
                const bool need_write    = value_changed
                                        || (!s_applied_ok && s_retry_ticks > 0)
                                        || (s_transition_ticks > 0);

                if (need_write) {
                    // Open aud:a only for the duration of the write then release
                    // immediately — MaxSessions=1 means holding it continuously
                    // blocks UltraGB from acquiring the service.
                    Result rc = audWrapperInitialize();
                    if (R_SUCCEEDED(rc)) {
                        rc = audWrapperSetProcessMasterVolume(current_pid, 0, v);
                        // audWrapperSetProcessRecordVolume(current_pid, 0, v);
                        audWrapperExit();
                    }
                    if (R_SUCCEEDED(rc)) {
                        s_last_vol    = v;
                        s_applied_ok  = true;
                        s_retry_ticks = 0;
                        // Do NOT zero s_transition_ticks on success — the system
                        // may still reset audproc after this write.
                    } else if (s_retry_ticks > 0) {
                        --s_retry_ticks;
                    } else if (s_transition_ticks == 0) {
                        s_last_vol = v;              // give up outside any active window
                    }
                    if (s_transition_ticks > 0) --s_transition_ticks;
                }
                s_in_vol_transition = (s_transition_ticks > 0);

                /* ---- Audproc watchdog ----
                 *
                 * Some system events (battery/power-cord notifications,
                 * friend-card overlays, etc.) reset the foreground process's
                 * audproc to 1.0 WITHOUT generating a pdmqry InFocus event.
                 * Because the focus-transition path is never triggered, the
                 * 1.0 sticks indefinitely.
                 *
                 * Every ~5 s in steady state, read the current audproc value.
                 * If it is at the system default of 1.0 AND sys-tune's
                 * configured level for this title is something other than 1.0,
                 * the delta is unambiguously a system reset (UltraGB would
                 * never write 1.0 intentionally).  Trigger a one-shot
                 * correction via the normal retry path.
                 *
                 * The read opens aud:a briefly (~µs) every 5 s — negligible
                 * IPC overhead.  The watchdog is suppressed during any active
                 * write window (transition or retry) to avoid redundancy. */
                static int s_watchdog_ticks = 0;
                if (s_applied_ok && s_transition_ticks == 0 && s_retry_ticks == 0
                        && std::fabs(v - 1.0f) > 0.01f) {
                    if (++s_watchdog_ticks >= 500) {   // ~5 s at 10 ms/tick
                        s_watchdog_ticks = 0;
                        float measured = -1.f;
                        if (R_SUCCEEDED(audWrapperInitialize())) {
                            audWrapperGetProcessMasterVolume(current_pid, &measured);
                            audWrapperExit();
                        }
                        if (measured >= 0.f && std::fabs(measured - 1.0f) < 0.005f) {
                            /* audproc is at system default but we want something
                             * else — looks like a silent system reset.  Re-apply. */
                            s_applied_ok  = false;
                            s_retry_ticks = 30;
                            s_last_vol    = -1.f;
                        }
                    }
                } else {
                    s_watchdog_ticks = 0;   // reset counter during active write windows
                }
            }

            /* ---- Advance pending stability window ---- */
            if (fade_pending_ticks > 0) {
                if (--fade_pending_ticks == 0) {
                    /* Window expired without OutOfFocus — game is genuinely
                     * in focus.  Start the deferred fade now.  Section (2)
                     * continues to run every 5 ms and can still reverse it
                     * if the user presses HOME after the game has loaded. */
                    if (fade_pending_action == StartAction::ForcePause) {
                        startFadeOut(kFadeSteps, /*do_pause=*/true, /*hold_ticks=*/600);
                    }
                    fade_pending_action = StartAction::DoNothing;
                }
            }

            /* ---- Advance non-blocking fade by one step ---- */
            if (fade_dir == FadeDir::Out) {
                audoutSetAudioOutVolume(fade_vol * float(fade_step) / fade_total);
                if (fade_step > 0) {
                    --fade_step;
                } else {
                    /* Fade-out complete. */
                    fade_dir = FadeDir::None;
                    if (fade_pause) {
                        if (fade_hold_target > 0) {
                            /* Enter the silent-hold phase: volume is already 0,
                             * music is still technically playing (policyWrite(true)
                             * has NOT been called yet).  Section (2) keeps running
                             * every 5 ms.  If pdmqry delivers OutOfFocus during the
                             * hold, startFadeIn cancels it and ramps straight back up
                             * — no audible pause.  Only when the hold expires does
                             * policyWrite(true) fire and the normal drain begin. */
                            fade_hold        = fade_hold_target;
                            fade_hold_target = 0;
                        } else {
                            policyWrite(true);
                            /* Hold silence while already-queued audio buffers drain
                             * (avoids a pop when music resumes).  Expressed in 5ms
                             * ticks to match the active-fade sleep cadence. */
                            fade_drain = (3 * AUDIO_LATENCY_MS + 4) / 5;
                            /* Volume intentionally left at 0 during drain;
                             * restored to fade_vol when drain expires below. */
                        }
                    } else {
                        audoutSetAudioOutVolume(fade_vol); /* no-pause fade: restore */
                    }
                }
            } else if (fade_dir == FadeDir::In) {
                audoutSetAudioOutVolume(fade_vol * float(fade_step) / fade_total);
                if (fade_step < fade_total) {
                    ++fade_step;
                } else {
                    audoutSetAudioOutVolume(fade_vol); /* snap to exact target */
                    fade_dir = FadeDir::None;
                }
            } else if (fade_hold > 0) {
                /* Silent-hold phase: volume stays at 0, music is still playing.
                 * Section (2) polls pdmqry every 5 ms throughout — if OutOfFocus
                 * arrives, startFadeIn cancels the hold and ramps back up.
                 * When the counter hits 0, commit the pause and start the drain. */
                if (--fade_hold == 0) {
                    policyWrite(true);
                    fade_drain = (3 * AUDIO_LATENCY_MS + 4) / 5;
                }
            } else if (fade_drain > 0) {
                if (--fade_drain == 0) {
                    audoutSetAudioOutVolume(fade_vol); /* restore after drain */
                }
            }

            // Three-tier sleep cadence:
            //   5 ms  — active fade: one step per tick, matches old blocking rate
            //   1 ms  — tight post-focus-flip window: catch audproc resets within 1 ms
            //  10 ms  — active write window (retry or sustained transition ticks)
            //  50 ms  — true steady state: no transitions, no retries pending.
            //            Title changes and HOME events are unaffected — they are
            //            detected within 50 ms at most, which is imperceptible.
            const u64 sleep_ns = fadeActive()              ? kFadeStepIntervalNs
                               : s_in_vol_transition       ? 1'000'000ULL
                               : (s_retry_ticks > 0 ||
                                  s_transition_ticks > 0)  ? 10'000'000ULL
                                                           : 50'000'000ULL;
            svcSleepThread(sleep_ns);
        }
    }

    bool GetStatus() {
        return !g_should_pause;
    }

    void Play() {
        g_should_pause       = false;
        g_user_paused        = false;  // clears the pause-veto
        g_saved_pause_state  = false;  // keep focus-suspend snapshot in sync
        leventSignal(&g_unpause_event);
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
        leventSignal(&g_unpause_event); // wake PlayTrack whether pausing or advancing
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
        leventSignal(&g_unpause_event);
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
        leventSignal(&g_unpause_event); // wake PlayTrack so it observes FetchNext
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
        g_should_pause        = false;
        g_user_paused        = false;
        g_saved_pause_state  = false;
        leventSignal(&g_unpause_event);
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

        // Wake TuneThreadFunc if it was blocked on an empty queue.
        leventSignal(&g_queue_changed_event);

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

        if (fetch_new)
            leventSignal(&g_unpause_event); // wake PlayTrack so it observes FetchNext

        return 0;
    }

}