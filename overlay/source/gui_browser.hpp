#pragma once

#include <tesla.hpp>
#include <string>
#include <functional>

#include "gui_base.hpp"
#include "elm_overlayframe.hpp"

class BrowserGui final : public SysTuneGui {
  private:
    SysTuneOverlayFrame     *m_frame;
    tsl::elm::List          *m_list;
    std::string              m_cwd;
    std::string              m_root;        // topmost browsable directory (never go above this)
    std::string              m_focus_name;  // list item name to restore focus to on nav-back
    std::vector<std::string> m_folder_songs; // paths of all files in m_cwd, shared across file item lambdas
    std::function<void(u32)> m_on_count_changed;
    u32                      m_depth = 1; // 1 at root, +1 per subdir level via changeTo

  public:
    // Default construction (entry point from SettingsGui) auto-detects /music/ vs /
    // Subsequent constructions via swapTo pass explicit path, focus, root, and callback
    BrowserGui(std::string path = "", std::string focus_name = "", std::string root = "",
               std::function<void(u32)> on_count_changed = nullptr, u32 depth = 1);
    ~BrowserGui();

    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;

  private:
    void buildList();
    void addAllToPlaylist(const std::string &path);

    // Fire m_on_count_changed with play_ctx::savedPlaylistSize().
    void notifyCountChanged() const;

    std::string parentPath()     const;
    std::string currentDirName() const;
};