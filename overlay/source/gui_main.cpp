#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "elm_volume.hpp"
#include "gui_browser.hpp"
#include "gui_playlist.hpp"
#include "play_context.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <functional>
#include <cstdio>

// =============================================================================
// PlayerRightDest + browser return path globals
// =============================================================================

static PlayerRightDest g_player_right_dest   = PlayerRightDest::Settings;
static std::string     g_browser_return_cwd;
static std::string     g_browser_return_root;

// ---------------------------------------------------------------------------
// Volume mute-backup persistence
// Three integers (0-100), one per line: Music, Game, Game (default).
// ---------------------------------------------------------------------------
static constexpr const char* kVolBackupFile = "/config/sys-tune/volume_backup.txt";

static void readVolBackups(u8 &music, u8 &game, u8 &game_def) {
    music = game = game_def = 100;
    FILE* f = fopen(kVolBackupFile, "r");
    if (!f) return;
    int m = 100, g = 100, gd = 100;
    fscanf(f, "%d\n%d\n%d", &m, &g, &gd);
    fclose(f);
    music    = static_cast<u8>(std::clamp(m,  0, 100));
    game     = static_cast<u8>(std::clamp(g,  0, 100));
    game_def = static_cast<u8>(std::clamp(gd, 0, 100));
}

static void writeVolBackups(u8 music, u8 game, u8 game_def) {
    FILE* f = fopen(kVolBackupFile, "w");
    if (!f) return;
    fprintf(f, "%d\n%d\n%d\n", static_cast<int>(music),
                                static_cast<int>(game),
                                static_cast<int>(game_def));
    fclose(f);
}

void setPlayerRightDest(PlayerRightDest dest) { g_player_right_dest = dest; }

void setBrowserReturnPath(const std::string& cwd, const std::string& root) {
    g_browser_return_cwd   = cwd;
    g_browser_return_root  = root;
}

// ---------------------------------------------------------------------------
// Tri-state focus action used by the "Title Focus" and "Custom Focus"
// cycling list items in SettingsGui.
//
//   Pass  — policy engine takes no action at title focus transitions
//           (music simply keeps doing whatever it was doing).
//   Play  — force music to play.
//   Pause — force music to pause.
//
// A KEY_A press cycles: Pass -> Play -> Pause -> Pass -> ...
//
// Storage is unchanged from the previous pair-of-toggles UI — each list
// item maps to two mutually-exclusive bool keys in config.ini — so
// existing user configs are fully backward compatible.
// ---------------------------------------------------------------------------
namespace {
    enum class FocusMode { Pass, Play, Pause };

    constexpr const char* focusLabel(FocusMode m) {
        switch (m) {
            case FocusMode::Play:  return "Play";
            case FocusMode::Pause: return "Pause";
            case FocusMode::Pass:
            default:               return "Pass";
        }
    }

    constexpr FocusMode focusNext(FocusMode m) {
        switch (m) {
            case FocusMode::Pass:  return FocusMode::Play;
            case FocusMode::Play:  return FocusMode::Pause;
            case FocusMode::Pause:
            default:               return FocusMode::Pass;
        }
    }
}

// ---------------------------------------------------------------------------
// Deferred SettingsGui rebuild — set by the "Default Focus" toggle listener
// and consumed at the top of SettingsGui::update() on the next frame.
//
// We CANNOT call tsl::swapTo from inside a setStateChangedListener because
// swapTo immediately destroys the current GUI (and therefore the element
// whose onClick is still on the call stack), causing a use-after-free on the
// triggerClickAnimation() and Element::onClick() calls that follow the
// listener invocation.  Deferring to update() lets the full input chain
// unwind safely before the rebuild fires.
// ---------------------------------------------------------------------------
static bool        s_settings_rebuild_pending = false;
static std::string s_settings_rebuild_jump;

// ---------------------------------------------------------------------------
// Return to the exact browser directory the user was in.
// The stack is always [SettingsGui, BrowserGui] — one changeTo is enough.
// Pressing B inside BrowserGui swaps to the parent directory, so the full
// tree is navigable without ever growing the stack beyond depth 2.
// ---------------------------------------------------------------------------
static void pushBrowserStack() {
    tsl::changeTo<BrowserGui>(
        g_browser_return_cwd, /*focus_name=*/"",
        g_browser_return_root, /*on_count_changed=*/nullptr);
}

// =============================================================================
// MainGui  (Page 0 — Player)
// =============================================================================

MainGui::MainGui() {
    // Initialise play context once on overlay open.
    // Loads persisted state from /config/sys-tune/ and snapshots IPC on first run.
    play_ctx::init();

    m_status_bar = new StatusBar();
}

MainGui::~MainGui() {
    // m_list cascade-deletes all its children, including m_status_bar.
    // m_frame is owned by Tesla — do NOT delete here.
    delete m_list;
}

// ---------------------------------------------------------------------------
tsl::elm::Element* MainGui::createUI() {
    m_right_label = (g_player_right_dest == PlayerRightDest::Playlist) ? "Playlist"
                  : (g_player_right_dest == PlayerRightDest::Browse)   ? "Browse"
                  : "Settings";
    m_frame = new SysTuneOverlayFrame(/*pageLeft=*/"", m_right_label);

    m_list = new tsl::elm::List();
    m_list->addItem(m_status_bar, StatusBar::PreferredHeight(tsl::cfg::FramebufferWidth - 85));

    m_status_bar->setPageRightCallback([] {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            /* swapTo replaces MainGui with SettingsGui, then changeTo stacks
               PlaylistGui on top: [SettingsGui, PlaylistGui].
               B on Playlist → [SettingsGui] → B closes. */
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            /* Return to the exact directory the user was browsing, not the
               currently-playing folder. */
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
    });

    // Pre-warm before the first draw so m_playing and m_percentage are already
    // correct on frame 0 — prevents the 0:00 flicker when swapping back from Settings.
    m_status_bar->update();

    m_frame->setContent(m_list);
    return m_frame;
}

// ---------------------------------------------------------------------------
void MainGui::update() {
    //static u8 tick = 0;
    //if ((tick % 15) == 0)
    m_status_bar->update();
    //if ((tick % 15) == 8)   // stagger from status_bar: fires midway between its updates
    play_ctx::poll();
    //++tick;

    // Detect shuffle mode changes and immediately resync g_saved to the
    // service's new queue order (the IPC call is synchronous so by the time
    // CycleShuffle() returns, the service has already reshuffled the queue).
    // Without this, g_saved stays in the pre-shuffle order until the user
    // opens PlaylistGui, causing index-based operations to use wrong positions.
    if (play_ctx::source() == play_ctx::Source::Playlist) {
        static TuneShuffleMode s_last_shuffle = TuneShuffleMode_Off;
        TuneShuffleMode cur = TuneShuffleMode_Off;
        //if ((tick % 15) == 1) {  // stagger slightly from status_bar update
        tuneGetShuffleMode(&cur);
        if (cur != s_last_shuffle) {
            s_last_shuffle = cur;
            play_ctx::resyncFromIPC();
        }
        //}
    }

    const std::string newLabel =
        (g_player_right_dest == PlayerRightDest::Playlist) ? "Playlist" :
        (g_player_right_dest == PlayerRightDest::Browse)   ? "Browse"   : "Settings";
    if (newLabel != m_right_label) {
        m_right_label = newLabel;
        if (m_frame) m_frame->setPageNames("", m_right_label);
    }
}

// ---------------------------------------------------------------------------
bool MainGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                          HidAnalogStickState joyStickPosLeft,
                          HidAnalogStickState joyStickPosRight) {

    if (m_status_bar->hasFocus())
        m_status_bar->onHeld(keysHeld);
    else
        g_player_r_held.store(false, std::memory_order_release);

    if (ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)) {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
        return true;
    }

    if (SysTuneGui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight))
        return true;

    /* KEY_R + RIGHT/LEFT while status bar is focused: bypass cursor movement
       and navigate pages directly — same destinations as normal page nav. */
    if (m_status_bar->hasFocus() && (keysHeld & KEY_R)) {
        if ((keysDown & KEY_RIGHT) && !(keysHeld & ~KEY_RIGHT & ~KEY_R & ALL_KEYS_MASK)) {
            g_player_r_held.store(false, std::memory_order_release);
            if (g_player_right_dest == PlayerRightDest::Playlist) {
                play_ctx::poll();
                tsl::swapTo<SettingsGui>();
                tsl::changeTo<PlaylistGui>(nullptr);
            } else if (g_player_right_dest == PlayerRightDest::Browse) {
                play_ctx::poll();
                tsl::swapTo<SettingsGui>();
                pushBrowserStack();
            } else {
                tsl::swapTo<SettingsGui>();
            }
            triggerNavigationFeedback();
            return true;
        }
    }

    if ((keysDown & KEY_RIGHT)
        && !(keysHeld & ~KEY_RIGHT & ALL_KEYS_MASK)
        && !m_status_bar->hasFocus()
        && !(ult::onTrackBar.load(std::memory_order_acquire)
             && (ult::unlockedSlide.load(std::memory_order_acquire) || ult::allowSlide.load(std::memory_order_acquire))
             && !(keysHeld & KEY_R))) {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
        return true;
    }

    return false;
}

// =============================================================================
// SettingsGui  (Page 1 — Settings)
// =============================================================================

SettingsGui::~SettingsGui() {
    delete m_list;
}

// ---------------------------------------------------------------------------
void SettingsGui::refreshPlaylistCount(u32 count) {
    if (!m_queue_button) return;
    m_last_count = count;
    /* Don't clobber the INPROGRESS_SYMBOL that update() manages. */
    const bool playlistActive = (play_ctx::source() == play_ctx::Source::Playlist)
                              && (play_ctx::currentPath()[0] != '\0');
    if (!playlistActive)
        m_queue_button->setValue(count == 1 ? "1 track" : std::to_string(count) + " tracks");
}

// ---------------------------------------------------------------------------
tsl::elm::Element* SettingsGui::createUI() {
    m_frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");

    u64 pid{}, tid{};
    pm::getCurrentPidTid(&pid, &tid);
    m_tid      = tid;
    m_last_tid = tid; // in sync from the start so update() only fires on changes

    // Format a title ID as a hex string for use as a label.
    auto tidLabel = [](u64 t) -> std::string {
        char buf[19];
        std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(t));
        return buf;
    };

    m_list = new tsl::elm::List();

    // ---- Music Selection ----
    m_list->addItem(new tsl::elm::CategoryHeader("Music Library"));

    /* Snapshot play_ctx state once so both buttons get the right initial value
       on frame 0, before any update() tick fires. */
    const bool init_inPlaylist = (play_ctx::source() == play_ctx::Source::Playlist);
    const bool init_inFolder   = (play_ctx::source() == play_ctx::Source::Folder);
    const bool init_hasTrack   = (play_ctx::currentPath()[0] != '\0');

    {
        const u32 count = play_ctx::savedPlaylistSize();
        m_last_count    = count;
        const std::string queueVal = (init_inPlaylist && init_hasTrack)
            ? ult::INPROGRESS_SYMBOL
            : (count == 1 ? "1 track" : std::to_string(count) + " tracks");
        m_queue_button = new tsl::elm::ListItem("Playlist", queueVal);
    }

    m_queue_button->setClickListener([this](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(m_queue_button);
            tsl::changeTo<PlaylistGui>([this](u32 count) {
                refreshPlaylistCount(count);
            });
            return true;
        }
        return false;
    });
    m_list->addItem(m_queue_button);

    const std::string browseVal = (init_inFolder && init_hasTrack)
        ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL;
    auto browser_button = new tsl::elm::ListItem("Browse", browseVal);
    m_browser_button = browser_button;
    browser_button->setClickListener([this, browser_button](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(browser_button);
            tsl::changeTo<BrowserGui>("", "", "", [this](u32 count) {
                refreshPlaylistCount(count);
            });
            return true;
        }
        return false;
    });
    m_list->addItem(browser_button);

    // ---- Volume ----
    m_list->addItem(new tsl::elm::CategoryHeader("Volume "+ult::DIVIDER_SYMBOL+" \uE0E3 Toggle Mute"));

    float tune_volume = 1.f, title_volume = 1.f, default_title_volume = 1.f;
    tuneGetVolume(&tune_volume);
    tuneGetTitleVolume(&title_volume);
    tuneGetDefaultTitleVolume(&default_title_volume);

    // Load persisted pre-mute backups so Y-toggle survives overlay reopen.
    readVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);

    // Initialise current-vol tracking from actual IPC values.
    m_music_vol        = static_cast<u8>(std::clamp(static_cast<int>(tune_volume          * 100.f + 0.5f), 0, 100));
    m_game_vol         = static_cast<u8>(std::clamp(static_cast<int>(title_volume         * 100.f + 0.5f), 0, 100));
    m_game_default_vol = static_cast<u8>(std::clamp(static_cast<int>(default_title_volume * 100.f + 0.5f), 0, 100));

    // Helper: same mute-toggle logic as the Y-key handler, usable from icon taps.
    // Captures SettingsGui members by pointer so the lambda stays copyable.
    auto makeMuteTap = [this](VolumeTrackBar** sliderPtr, u8* vol, u8* backup,
                               std::function<void(u8)> applyFn) -> std::function<void()> {
        return [this, sliderPtr, vol, backup, applyFn]() {
            auto* slider = *sliderPtr;
            if (!slider) return;
            if (*vol > 0) {
                *backup = *vol;
                *vol    = 0;
            } else {
                *vol = (*backup > 0) ? *backup : static_cast<u8>(100);
            }
            slider->setProgress(*vol);
            applyFn(*vol);
            writeVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);
        };
    };

    auto tune_volume_slider = new VolumeTrackBar("\uE13C", false, false, true, "Music", "%", false);
    tune_volume_slider->setProgress(tune_volume * 100);
    tune_volume_slider->setValueChangedListener([this](u8 value) {
        m_music_vol = value;
        tuneSetVolume(float(value) / 100.f);
    });
    m_music_slider = tune_volume_slider;
    tune_volume_slider->setIconTapCallback(makeMuteTap(
        &m_music_slider, &m_music_vol, &m_music_vol_backup,
        [](u8 v) { tuneSetVolume(float(v) / 100.f); }));
    m_list->addItem(tune_volume_slider);
    
    if (tid && pid) {
        auto title_volume_slider = new VolumeTrackBar("\uE13C", false, false, true, "Game", "%", false);
        title_volume_slider->setProgress(title_volume * 100);
        title_volume_slider->setValueChangedListener([this](u8 value) {
            m_game_vol = value;
            const float v = float(value) / 100.f;
            tuneSetTitleVolume(v);
            config::set_title_volume(m_tid, v);
        });
        m_game_slider = title_volume_slider;
        title_volume_slider->setIconTapCallback(makeMuteTap(
            &m_game_slider, &m_game_vol, &m_game_vol_backup,
            [this](u8 v) {
                const float fv = float(v) / 100.f;
                tuneSetTitleVolume(fv);
                config::set_title_volume(m_tid, fv);
            }));
        m_list->addItem(title_volume_slider);
    }
    

    // ---- Auto Play ----
    // The HOME menu is a peculiar "title":
    //   - It has no per-game volume to tune, so "Preset Volume" is a no-op.
    //   - Its tid is a well-known constant rather than meaningful per-game info,
    //     so the "Title ID" header is noise.
    //   - Default Focus / Custom Focus are irrelevant at HOME because HOME
    //     always has an explicit per-title entry (set via the dedicated
    //     "Pause On Home" toggle added below).
    // Hide those widgets when the current tid is HOME.
    constexpr u64 kHomeScreenTid = 0x0100000000001000ULL;
    const bool at_home = (tid == kHomeScreenTid);

    if (!at_home) {
        auto* defaultTitleCategoryHeader = new tsl::elm::CategoryHeader("Title ID");
        defaultTitleCategoryHeader->setValue(tidLabel(tid), tsl::onTextColor);
        m_list->addItem(defaultTitleCategoryHeader);

        auto default_title_volume_slider = new VolumeTrackBar("\uE13C", false, false, true, "Preset Volume", "%", false);
        {
            float per_title_vol = 1.f;
            tuneGetDefaultTitleVolume(&per_title_vol);
            if (tid) per_title_vol = config::get_title_volume(tid);
            default_title_volume_slider->setProgress(static_cast<u8>(std::clamp(per_title_vol * 100.f + 0.5f, 0.f, 100.f)));
            m_game_default_vol = default_title_volume_slider->getProgress();
        }
        default_title_volume_slider->setValueChangedListener([this](u8 value) {
            m_game_default_vol = value;
            const float fv = float(value) / 100.f;
            tuneSetDefaultTitleVolume(fv);
            if (m_tid) config::set_title_volume(m_tid, fv);
        });
        m_game_default_slider = default_title_volume_slider;
        default_title_volume_slider->setIconTapCallback(makeMuteTap(
            &m_game_default_slider, &m_game_default_vol, &m_game_default_vol_backup,
            [this](u8 v) {
                const float fv = float(v) / 100.f;
                tuneSetDefaultTitleVolume(fv);
                if (m_tid) config::set_title_volume(m_tid, fv);
            }));
        m_list->addItem(default_title_volume_slider);

        // ---- Per-title start-up policy ----
        //
        // Two widgets work together:
        //
        //   Default Focus  (toggle) — when ON (the factory default), this
        //                             title defers to the global "Title
        //                             Focus" setting in Miscellaneous.
        //                             Turn it OFF to expose the per-title
        //                             "Custom Focus" override below.
        //
        //   Custom Focus  (tri-state cycle: Pass / Play / Pause) — only
        //                             shown when Default Focus is OFF.
        //                             Each KEY_A press cycles the state.
        //                             Storage:
        //
        //                               Pass  -> both keys absent
        //                               Play  -> title_enabled(tid) = true
        //                               Pause -> title_pause_on_start(tid) = true
        //
        // The underlying config keys (title_enabled /
        // title_pause_on_start / default_on_start) are unchanged from the
        // previous pair-of-toggles UI, so existing configs are fully
        // backward compatible.
        //
        // Toggling Default Focus does a deferred swapTo<SettingsGui> so
        // the list is rebuilt immediately: the Custom Focus row appears
        // or disappears without any stale state.
        const bool init_default_focus = config::get_default_on_start(tid);
        auto default_focus = new tsl::elm::ToggleListItem(
            "Default Focus", init_default_focus, "On", "Off");
        default_focus->setStateChangedListener([tid](bool v) {
            config::set_default_on_start(tid, v);
            // Defer the rebuild to SettingsGui::update() so the full
            // onClick/handleInput call stack unwinds before swapTo fires.
            // Calling swapTo here directly would destroy the element while
            // its own onClick is still on the stack (use-after-free).
            s_settings_rebuild_pending = true;
            s_settings_rebuild_jump    = "Default Focus";
        });
        m_list->addItem(default_focus);

        // Only expose the per-title override when the user has opted out
        // of the global default — keeps the list clean by default.
        if (!init_default_focus) {
            // Pause takes priority over Play if both keys are somehow set
            // (matches resolvePerTitlePolicy's per-title order in
            // music_player.cpp).
            FocusMode initial = FocusMode::Pass;
            if (config::has_title_pause_on_start(tid) && config::get_title_pause_on_start(tid))
                initial = FocusMode::Pause;
            else if (config::has_title_enabled(tid) && config::get_title_enabled(tid))
                initial = FocusMode::Play;

            auto *custom_focus = new tsl::elm::ListItem("Custom Focus", focusLabel(initial));
            // Render "Pass" faintly so it reads as "inactive", mirroring
            // how a ToggleListItem renders its OFF value.
            custom_focus->setValue(focusLabel(initial), initial == FocusMode::Pass);
            custom_focus->setClickListener(
                [custom_focus, state = initial, tid](u64 keys) mutable -> bool {
                    if (keys & HidNpadButton_A) {
                        state = focusNext(state);
                        switch (state) {
                            case FocusMode::Play:
                                config::set_title_enabled(tid, true);
                                config::clear_title_pause_on_start(tid);
                                break;
                            case FocusMode::Pause:
                                config::clear_title_enabled(tid);
                                config::set_title_pause_on_start(tid, true);
                                break;
                            case FocusMode::Pass:
                            default:
                                config::clear_title_enabled(tid);
                                config::clear_title_pause_on_start(tid);
                                break;
                        }
                        custom_focus->setValue(focusLabel(state),
                                               state == FocusMode::Pass);
                        return true;
                    }
                    return false;
                });
            m_list->addItem(custom_focus);
        }
    }

    // ---- Misc ----
    m_list->addItem(new tsl::elm::CategoryHeader("Miscellaneous"));

    // Title Focus — global default applied to any title whose per-title
    // "Default Focus" is ON (the factory default). Cycling tri-state:
    //
    //   Pass  — no policy action on title focus transitions
    //           (music simply keeps doing whatever it was doing).
    //   Play  — force music to play on every title focus transition.
    //   Pause — force music to pause on every title focus transition.
    //
    // Storage is unchanged (two mutually-exclusive bool keys,
    // play_on_title / pause_on_title). The cycling widget just replaces
    // the previous pair of ON/OFF toggles with a single list item.
    //
    // Sits at the top of Miscellaneous so the user can set the
    // system-wide behaviour before drilling into per-title overrides.
    {
        // Pause takes priority over Play if both keys are somehow set
        // (matches resolvePerTitlePolicy's global order in music_player.cpp).
        FocusMode initial = FocusMode::Pass;
        if (config::get_pause_on_title())
            initial = FocusMode::Pause;
        else if (config::get_play_on_title())
            initial = FocusMode::Play;

        auto *title_focus = new tsl::elm::ListItem("Title Focus", focusLabel(initial));
        title_focus->setValue(focusLabel(initial), initial == FocusMode::Pass);
        title_focus->setClickListener(
            [title_focus, state = initial](u64 keys) mutable -> bool {
                if (keys & HidNpadButton_A) {
                    state = focusNext(state);
                    switch (state) {
                        case FocusMode::Play:
                            config::set_play_on_title(true);
                            config::set_pause_on_title(false);
                            break;
                        case FocusMode::Pause:
                            config::set_play_on_title(false);
                            config::set_pause_on_title(true);
                            break;
                        case FocusMode::Pass:
                        default:
                            config::set_play_on_title(false);
                            config::set_pause_on_title(false);
                            break;
                    }
                    title_focus->setValue(focusLabel(state),
                                          state == FocusMode::Pass);
                    return true;
                }
                return false;
            });
        m_list->addItem(title_focus);
    }

    // Home Focus — cycling tri-state for the HOME menu press.
    // Always visible regardless of what title is running.
    //
    //   Pass  → no action when pressing HOME (music keeps its current state).
    //   Play  → music plays  when pressing HOME.
    //   Pause → music pauses when pressing HOME.
    //
    // Storage uses the same two mutually-exclusive keys as Custom Focus:
    //   Play  → title_enabled(kHomeScreenTid)        = true
    //   Pause → title_pause_on_start(kHomeScreenTid) = true
    //   Pass  → both keys absent
    {
        FocusMode home_initial = FocusMode::Pass;
        if (config::has_title_pause_on_start(kHomeScreenTid) && config::get_title_pause_on_start(kHomeScreenTid))
            home_initial = FocusMode::Pause;
        else if (config::has_title_enabled(kHomeScreenTid) && config::get_title_enabled(kHomeScreenTid))
            home_initial = FocusMode::Play;

        auto *home_focus = new tsl::elm::ListItem("Home Focus", focusLabel(home_initial));
        home_focus->setValue(focusLabel(home_initial), home_initial == FocusMode::Pass);
        home_focus->setClickListener(
            [home_focus, state = home_initial](u64 keys) mutable -> bool {
                if (keys & HidNpadButton_A) {
                    state = focusNext(state);
                    switch (state) {
                        case FocusMode::Play:
                            config::set_title_enabled(kHomeScreenTid, true);
                            config::clear_title_pause_on_start(kHomeScreenTid);
                            break;
                        case FocusMode::Pause:
                            config::clear_title_enabled(kHomeScreenTid);
                            config::set_title_pause_on_start(kHomeScreenTid, true);
                            break;
                        case FocusMode::Pass:
                        default:
                            config::clear_title_enabled(kHomeScreenTid);
                            config::clear_title_pause_on_start(kHomeScreenTid);
                            break;
                    }
                    home_focus->setValue(focusLabel(state), state == FocusMode::Pass);
                    return true;
                }
                return false;
            });
        m_list->addItem(home_focus);
    }

    // Auto-play Startup — applies to the sysmodule itself, not any
    // individual title. When ON, music begins playing as soon as the
    // sysmodule starts up (assuming a startup playlist is set),
    // regardless of which title is in foreground at boot time.
    auto auto_play_startup = new tsl::elm::ToggleListItem(
        "Auto-play Startup",
        config::get_auto_play_startup(),
        "On", "Off");
    auto_play_startup->setStateChangedListener([](bool v) {
        config::set_auto_play_startup(v);
    });
    m_list->addItem(auto_play_startup);

    auto startup_button = new tsl::elm::ListItem("Remove Startup");
    startup_button->setClickListener([this, startup_button](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(startup_button);
            char path[512];
            if (config::get_load_path(path, sizeof(path))) {
                config::set_load_path("");
                const char *p = path;
                if (const char *ext = std::strrchr(path, '/')) p = ext + 1;
                if (tsl::notification) tsl::notification->showNow(p, 26, "Startup Path Removed", 2500, false);
            } else {
                if (tsl::notification) tsl::notification->showNow("No startup path set in config.");
            }
            return true;
        }
        return false;
    });
    m_list->addItem(startup_button);

    auto exit_button = new tsl::elm::SilentListItem("Stop sys-tune");
    exit_button->setValue("\uE071", true);
    exit_button->setClickListener([exit_button](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(exit_button);
            tuneQuit();
            tsl::goBack();
            return true;
        }
        return false;
    });
    m_list->addItem(exit_button);

    m_frame->setContent(m_list);

    // Auto-jump to whichever music source is actively playing so the user
    // lands directly on the relevant button rather than the first item.
    if (!m_jump_to.empty()) {
        m_list->jumpToItem(m_jump_to);
    } else if (init_inPlaylist && init_hasTrack) {
        m_list->jumpToItem("Playlist");
    } else if (init_inFolder && init_hasTrack) {
        m_list->jumpToItem("Browse");
    }

    return m_frame;
}

// ---------------------------------------------------------------------------
void SettingsGui::update() {
    // Consume any pending rebuild request.  This is set by the "Default On
    // Start" listener and intentionally deferred here so the swapTo fires
    // after the full onClick/handleInput chain has returned — not while the
    // toggle element is still mid-callback.
    if (s_settings_rebuild_pending) {
        s_settings_rebuild_pending = false;
        tsl::swapTo<SettingsGui>(std::move(s_settings_rebuild_jump));
        return;
    }

    /* Poll IPC on a throttled schedule — it's a syscall.
       Button label updates run every tick so values snap back instantly
       the moment SettingsGui resumes after a child GUI is popped. */
    //static u8 tick = 0;
    //if ((++tick % 15) == 0)
    play_ctx::poll();

    // Re-check the running title every tick (cheap — just reads globals set by
    // pm::getCurrentPidTid which was already called recently by play_ctx::poll).
    {
        u64 pid{}, tid{};
        pm::getCurrentPidTid(&pid, &tid);
        if (tid != m_last_tid) {
            m_tid      = tid;
            m_last_tid = tid;

            // Format the new title ID.
            char buf[19];
            std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(tid));
            const std::string label(buf);

            // Refresh the "Game (default)" slider — relabel and reload preset.
            if (m_game_default_slider) {
                m_game_default_slider->setLabel(label);
                float vol = 1.f;
                tuneGetDefaultTitleVolume(&vol);
                if (tid) vol = config::get_title_volume(tid);
                const u8 pct = static_cast<u8>(std::clamp(vol * 100.f + 0.5f, 0.f, 100.f));
                m_game_default_slider->setProgress(pct);
                m_game_default_vol = pct;
            }
        }
    }

    const bool inPlaylist = (play_ctx::source() == play_ctx::Source::Playlist);
    const bool inFolder   = (play_ctx::source() == play_ctx::Source::Folder);
    const bool hasTrack   = (play_ctx::currentPath()[0] != '\0');

    if (m_queue_button) {
        if (inPlaylist && hasTrack) {
            m_queue_button->setValue(ult::INPROGRESS_SYMBOL);
        } else {
            const u32 count = play_ctx::savedPlaylistSize();
            m_last_count = count;
            m_queue_button->setValue(count == 1 ? "1 track" : std::to_string(count) + " tracks");
        }
    }

    if (m_browser_button) {
        m_browser_button->setValue(
            (inFolder && hasTrack) ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL);
    }
}

// ---------------------------------------------------------------------------
bool SettingsGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                              HidAnalogStickState joyStickPosLeft,
                              HidAnalogStickState joyStickPosRight) {

    if (ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)) {
        setPlayerRightDest(PlayerRightDest::Settings);
        tsl::swapTo<MainGui>();
        triggerNavigationFeedback();
        return true;
    }

    // Y — mute/restore toggle on the focused volume slider.
    // If the slider is at any non-zero value, back it up and set to 0.
    // If it is already at 0, restore from the backup (or 100 if no backup).
    // The backup values are written to disk so they survive overlay reopen.
    if (keysDown & KEY_Y) {
        auto doMuteToggle = [&](VolumeTrackBar* slider, u8& vol, u8& backup,
                                std::function<void(u8)> applyFn) -> bool {
            if (!slider || !slider->hasFocus()) return false;
            if (vol > 0) {
                backup = vol;           // store current level
                vol    = 0;
            } else {
                vol = (backup > 0) ? backup : static_cast<u8>(100);  // restore
            }
            slider->setProgress(vol);
            applyFn(vol);
            writeVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);
            triggerNavigationFeedback();
            return true;
        };

        if (doMuteToggle(m_music_slider, m_music_vol, m_music_vol_backup,
            [](u8 v) { tuneSetVolume(float(v) / 100.f); }))
            return true;

        if (doMuteToggle(m_game_slider, m_game_vol, m_game_vol_backup,
            [this](u8 v) {
                const float fv = float(v) / 100.f;
                tuneSetTitleVolume(fv);
                config::set_title_volume(m_tid, fv);
            }))
            return true;

        if (doMuteToggle(m_game_default_slider, m_game_default_vol, m_game_default_vol_backup,
            [](u8 v) { tuneSetDefaultTitleVolume(float(v) / 100.f); }))
            return true;
    }

    if (SysTuneGui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight))
        return true;

    if ((keysDown & KEY_LEFT)
        && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK)
        && !(ult::onTrackBar.load(std::memory_order_acquire)
             && (ult::unlockedSlide.load(std::memory_order_acquire) || ult::allowSlide.load(std::memory_order_acquire)))) {
        setPlayerRightDest(PlayerRightDest::Settings);
        tsl::swapTo<MainGui>();
        triggerNavigationFeedback();
        return true;
    }
    return false;
}