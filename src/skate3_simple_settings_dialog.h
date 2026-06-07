#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

void EnsureSkate3SimpleSettingsConfig(const std::filesystem::path& config_path);
void SaveSkate3SimpleSettingsConfig(const std::filesystem::path& config_path);

class Skate3SimpleSettingsDialog final : public rex::ui::ImGuiDialog {
 public:
  using LoadProfilesCallback = std::function<rex::ui::SimpleProfileState()>;
  using SaveProfileCallback =
      std::function<void(int selected_index, std::string gamertag, bool signed_in)>;
  using CloseSettingsCallback = std::function<void()>;
  using CloseGameCallback = std::function<void()>;
  using RestartGameCallback = std::function<void()>;

  Skate3SimpleSettingsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                             LoadProfilesCallback load_profiles,
                             SaveProfileCallback save_profile,
                             CloseSettingsCallback close_settings,
                             CloseGameCallback close_game,
                             RestartGameCallback restart_game);
  ~Skate3SimpleSettingsDialog();

  void Show();
  void Toggle();
  void Hide();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void LoadSettingsFromCvars();
  bool HasSettingsChanges() const;
  void ReloadProfiles();
  void SaveVideo();
  void SaveProfile();

  std::filesystem::path config_path_;
  LoadProfilesCallback load_profiles_;
  SaveProfileCallback save_profile_;
  CloseSettingsCallback close_settings_;
  CloseGameCallback close_game_;
  RestartGameCallback restart_game_;
  rex::ui::SimpleProfileState profiles_;
  bool visible_ = false;
  int resolution_scale_index_ = 0;
  int frame_cap_index_ = 0;
  int input_backend_index_ = 0;
  bool fullscreen_ = true;
  bool vsync_ = false;
  bool tearing_ = true;
  bool mnk_mode_ = false;
  bool mnk_capture_mouse_ = false;
  int selected_tab_ = 0;
  bool profile_signed_in_ = true;
  char gamertag_buf_[32] = {};
};
