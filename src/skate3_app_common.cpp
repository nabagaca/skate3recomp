#include "skate3_app_common.h"

#include "skate3_iso_installer.h"
#include "skate3_user_settings.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#if defined(__linux__) || defined(__APPLE__)
extern char** environ;
#endif

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

// Register multi-entry-function alternate entries that rex's analyzer can't
// lift via config because they overlap with auto-discovered parent functions.
extern "C" REX_FUNC(__restgprlr_19);

namespace {

#if defined(__linux__) || defined(__APPLE__)
std::vector<std::string> CurrentProcessArgumentsForRestart(
    const std::filesystem::path& executable_path) {
  std::vector<std::string> args;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  args.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i] ? argv[i] : "");
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  char ch = 0;
  while (cmdline.get(ch)) {
    if (ch == '\0') {
      args.push_back(std::move(arg));
      arg.clear();
    } else {
      arg.push_back(ch);
    }
  }
  if (!arg.empty()) {
    args.push_back(std::move(arg));
  }
#endif

  const std::string executable = rex::path_to_utf8(executable_path);
  if (args.empty()) {
    args.push_back(executable);
  } else {
    args[0] = executable;
  }
  return args;
}

void SetRestartArgument(std::vector<std::string>& args, std::string name, std::string value) {
  const std::string option = "--" + name;
  const std::string option_with_equals = option + "=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == option) {
      if (i + 1 < args.size()) {
        args[i + 1] = std::move(value);
      } else {
        args.push_back(std::move(value));
      }
      return;
    }
    if (args[i].starts_with(option_with_equals)) {
      args[i] = option_with_equals + value;
      return;
    }
  }

  args.push_back(std::move(option));
  args.push_back(std::move(value));
}
#endif

constexpr std::string_view kUserDirectoryName = "skate3";
constexpr std::string_view kSettingsFilename = "settings.toml";

std::filesystem::path DefaultDocumentsUserRoot() {
  return rex::filesystem::GetUserFolder() / std::string(kUserDirectoryName);
}

std::filesystem::path DefaultRoamingUserRoot() {
#if defined(_WIN32)
  char* appdata = nullptr;
  size_t appdata_size = 0;
  if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata && *appdata) {
    std::filesystem::path result = std::filesystem::path(appdata) / std::string(kUserDirectoryName);
    std::free(appdata);
    return result;
  }
  std::free(appdata);
#endif
  return DefaultDocumentsUserRoot();
}

std::filesystem::path ResolveSkate3UserRoot(const rex::PathConfig& paths) {
  const auto executable_root = rex::filesystem::GetExecutableFolder();
  if (std::filesystem::exists(executable_root / "portable.txt")) {
    return executable_root;
  }

  const auto old_default = DefaultDocumentsUserRoot();
  if (!paths.user_data_root.empty() && paths.user_data_root != old_default) {
    return paths.user_data_root;
  }

  return DefaultRoamingUserRoot();
}

void ConfigureSkate3UserPaths(rex::PathConfig& paths, std::filesystem::path& settings_path,
                              std::filesystem::path& profiles_path) {
  const auto old_user_root = DefaultDocumentsUserRoot();
  const auto old_cache_root = old_user_root / "cache";
  const auto original_cache_root = paths.cache_root;
  const auto resolved_user_root = ResolveSkate3UserRoot(paths);

  paths.user_data_root = resolved_user_root;
  if (original_cache_root.empty() || original_cache_root == old_cache_root) {
    paths.cache_root = resolved_user_root / "cache";
  }
  settings_path = resolved_user_root / std::string(kSettingsFilename);
  profiles_path = skate3::ProfilesFilePath(resolved_user_root);
}

bool ConfigContainsAnyKey(const toml::table& table, std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    if (table.contains(key)) {
      return true;
    }
  }
  return false;
}

bool ConfigFileContainsAnyKey(const std::filesystem::path& config_path,
                              std::initializer_list<std::string_view> keys) {
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return false;
  }
  try {
    auto config = toml::parse_file(config_path.string());
    return ConfigContainsAnyKey(config, keys);
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool DeveloperConfigHasResolutionScaleOverride(const std::filesystem::path& config_path) {
  return ConfigFileContainsAnyKey(config_path, {"resolution_scale", "draw_resolution_scale_x",
                                                "draw_resolution_scale_y"});
}

constexpr int kDefaultResolutionScale = 2;

void ApplyFirstRunVideoDefaults(const std::filesystem::path& settings_path,
                                const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path) ||
      DeveloperConfigHasResolutionScaleOverride(developer_config_path) ||
      rex::cvar::HasNonDefaultValue("resolution_scale") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_x") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")) {
    return;
  }

  const auto scale = std::to_string(kDefaultResolutionScale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
}

void LoadAndNormalizeSimpleSettings(const std::filesystem::path& settings_path,
                                    const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path)) {
    rex::cvar::LoadConfig(settings_path);
  } else {
    ApplyFirstRunVideoDefaults(settings_path, developer_config_path);
  }
  rex::ui::EnsureSimpleSettingsConfig(settings_path);
}

std::filesystem::path ResolveRuntimeGameDataRoot(const rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return paths.game_data_root;
  }

  const auto working_directory_game = std::filesystem::current_path() / "game";
  if (skate3::IsGameInstalled(working_directory_game)) {
    return working_directory_game;
  }

  return paths.config_path.parent_path() / "game";
}

const char* FirstExistingFontPath(std::initializer_list<const char*> paths) {
  for (const char* path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

}  // namespace

// sub_82EBAE4C: alternate entry into sub_82EBAE34 (37 vtable refs in .data).
// Body: lwz r3, 0x164(r31); addi r1, r31, 0x140; b __restgprlr_19
static void Sub82EBAE4CImpl(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0x164);
  ctx.r1.s64 = ctx.r31.s64 + 0x140;
  __restgprlr_19(ctx, base);
}

Skate3BaseApp::~Skate3BaseApp() = default;

void Skate3BaseApp::OnConfigurePaths(rex::PathConfig& paths) {
  ConfigureSkate3UserPaths(paths, user_settings_path_, profiles_path_);
  config_path_ = paths.config_path;
  LoadAndNormalizeSimpleSettings(user_settings_path_, config_path_);
}

void Skate3BaseApp::OnConfigureFonts(ImFontAtlas* atlas) {
  if (!atlas) {
    return;
  }

  atlas->Clear();
  ImFontConfig font_config;
  font_config.OversampleH = 2;
  font_config.OversampleV = 2;
  font_config.PixelSnapH = false;

  const char* base_font_path = FirstExistingFontPath({
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\Helvetica.ttf",
      "C:\\Windows\\Fonts\\helvetica.ttf",
      "C:\\Windows\\Fonts\\HelveticaNeue.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/local/share/fonts/NotoSans-Regular.ttf",
      "/usr/local/share/fonts/LiberationSans-Regular.ttf",
#endif
  });

  if (base_font_path) {
    atlas->AddFontFromFileTTF(base_font_path, 16.0f, &font_config,
                              atlas->GetGlyphRangesDefault());
  } else {
    atlas->AddFontDefault();
  }

#if defined(_WIN32)
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_font_config,
                              atlas->GetGlyphRangesJapanese());
  }
#endif
}

std::optional<rex::PathConfig> Skate3BaseApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  config_path_ = defaults.config_path;
  user_settings_path_ = defaults.user_data_root / std::string(kSettingsFilename);
  profiles_path_ = skate3::ProfilesFilePath(defaults.user_data_root);

  auto profiles = skate3::LoadProfiles(profiles_path_);
  const bool has_profiles_file = std::filesystem::exists(profiles_path_);
  skate3::EnsureUsableProfileStore(profiles, "Player");
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
  }

  const bool has_config_file = std::filesystem::exists(defaults.config_path);
  const bool has_game_path = std::filesystem::is_directory(defaults.game_data_root);
  if (!has_profiles_file && has_config_file && has_game_path) {
    skate3::SaveProfiles(profiles_path_, profiles);
  }
  auto runtime_paths = defaults;
  runtime_paths.game_data_root = ResolveRuntimeGameDataRoot(runtime_paths);
  runtime_paths.config_path.clear();
  if (!skate3::IsGameInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Game files not found at {}; launching rexglue ISO installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__)
    if (const char* automated_iso = std::getenv("SKATE3_INSTALL_ISO");
        automated_iso == nullptr || *automated_iso == '\0') {
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                          std::move(resume));
      return std::nullopt;
    }
#endif
    rex::PathConfig installed_paths;
    const bool installed = skate3::RunRexglueIsoInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, installed_paths);
    if (!installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    return installed_paths;
  }
  return runtime_paths;
}

void Skate3BaseApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  rex::ui::RegisterBind("bind_skate3_menu", "Escape", "Skate 3 settings", [this] {
    ToggleSimpleSettings();
  });
  rex::ui::RegisterBind("bind_skate3_menu_alt", "F1", "Skate 3 settings alternate", [this] {
    ToggleSimpleSettings();
  });
  rex::ui::RegisterBind("bind_skate3_save_draw_fingerprints", "F8",
                        "Save draw fingerprint log", [this] {
                          SaveDrawFingerprintLog();
                        });
  rex::ui::RegisterBind("bind_skate3_log_debug_marker", "F9",
                        "Write debug marker to log", [this] {
                          LogDebugMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_log_user_marker", "F10",
                        "Write marker to log", [this] {
                          LogUserMarker();
                        });
}

void Skate3BaseApp::OnPostSetup() {
  ApplySelectedProfileToRuntime();
  ApplyGameplayCursorMode();

  if (auto* input_system = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    input_system->SetActiveCallback([this]() {
      const bool settings_visible = simple_settings_dialog_ && simple_settings_dialog_->visible();
      const bool xam_ui_active = rex::kernel::xam::xeXamIsUIActive();
      return !settings_visible && !xam_ui_active;
    });
    input_system->SetMenuChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() { ToggleSimpleSettings(); });
    });
  }

  if (std::getenv("SKATE3_DISABLE_BIG_ALIASES") == nullptr) {
    InstallBigDeviceAliases();
  }
  InstallRecipeOverlay();

  // Register multi-entry-function alternate entries.
  runtime()->function_dispatcher()->SetFunction(0x82EBAE4C, &Sub82EBAE4CImpl);

  auto* dispatcher = runtime()->function_dispatcher();
  if (dispatcher->InitializeFunctionTable(eawebkit_PPCImageConfig.code_base,
                                          eawebkit_PPCImageConfig.code_size,
                                          eawebkit_PPCImageConfig.image_base,
                                          eawebkit_PPCImageConfig.image_size)) {
    for (int i = 0; eawebkit_PPCImageConfig.func_mappings[i].guest != 0; ++i) {
      auto* host = eawebkit_PPCImageConfig.func_mappings[i].host;
      if (host) {
        dispatcher->SetFunction(
            static_cast<uint32_t>(eawebkit_PPCImageConfig.func_mappings[i].guest),
            host);
      }
    }
  }
}

void Skate3BaseApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_skate3_menu");
  rex::ui::UnregisterBind("bind_skate3_menu_alt");
  rex::ui::UnregisterBind("bind_skate3_save_draw_fingerprints");
  rex::ui::UnregisterBind("bind_skate3_log_debug_marker");
  rex::ui::UnregisterBind("bind_skate3_log_user_marker");
  ApplyGameplayCursorMode();
  simple_settings_dialog_.reset();
}

void Skate3BaseApp::ToggleSimpleSettings() {
  if (simple_settings_dialog_) {
    if (simple_settings_dialog_->visible()) {
      simple_settings_dialog_->Hide();
    } else {
      ApplySettingsCursorMode();
      simple_settings_dialog_->Show();
    }
    return;
  }

  auto load_profiles = [this]() {
    rex::ui::SimpleProfileState state;
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    state.selected_index = 0;
    for (int i = 0; i < static_cast<int>(store.profiles.size()); ++i) {
      const auto& profile = store.profiles[i];
      state.profiles.push_back({profile.id, profile.gamertag, profile.signed_in});
      if (profile.id == store.selected_profile) {
        state.selected_index = i;
      }
    }
    return state;
  };
  auto save_profile = [this](int selected_index, std::string gamertag, bool signed_in) {
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    if (store.profiles.empty()) {
      return;
    }
    selected_index = std::clamp(selected_index, 0, static_cast<int>(store.profiles.size()) - 1);
    auto& profile = store.profiles[selected_index];
    if (!gamertag.empty()) {
      profile.gamertag = std::move(gamertag);
    }
    profile.signed_in = signed_in;
    if (!profile.signed_in) {
      profile.live_signed_in = false;
    }
    store.selected_profile = profile.id;
    skate3::SaveProfiles(profiles_path_, store);
    skate3::ApplyProfileCvars(profile);
    ApplySelectedProfileToRuntime();
  };
  auto close_settings = [this]() { ApplyGameplayCursorMode(); };
  auto close_game = [this]() {
#if REX_PLATFORM_MAC || REX_PLATFORM_LINUX
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      REXLOG_WARN("Close Game watchdog exiting process after shutdown timeout");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#endif
    app_context().CallInUIThreadDeferred([this]() {
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      if (window()) {
        window()->RequestClose();
      } else {
        app_context().QuitFromUIThread();
      }
#endif
    });
  };
  auto restart_game = [this]() { RestartGame(); };
  simple_settings_dialog_ =
      std::make_unique<rex::ui::SimpleSettingsDialog>(
          imgui_drawer(), user_settings_path_, std::move(load_profiles), std::move(save_profile),
          std::move(close_settings), std::move(close_game), std::move(restart_game));
  ApplySettingsCursorMode();
  simple_settings_dialog_->Show();
}

void Skate3BaseApp::ApplySettingsCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void Skate3BaseApp::ApplyGameplayCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void Skate3BaseApp::RestartGame() {
  app_context().CallInUIThreadDeferred([this]() {
#if defined(_WIN32)
    wchar_t executable_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup_info, &process_info)) {
      REXLOG_WARN("Restart requested, but launching a new process failed");
      return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#elif defined(__linux__) || defined(__APPLE__)
    const auto executable_path = rex::filesystem::GetExecutablePath();
    if (executable_path.empty()) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    auto args = CurrentProcessArgumentsForRestart(executable_path);
    std::filesystem::path restart_game_data_root;
    if (runtime()) {
      restart_game_data_root = runtime()->game_data_root();
    }
    if (restart_game_data_root.empty()) {
      restart_game_data_root = game_data_root();
    }
    if (!restart_game_data_root.empty()) {
      SetRestartArgument(args, "game_data_root", rex::path_to_utf8(restart_game_data_root));
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const std::string executable = rex::path_to_utf8(executable_path);
    pid_t child_pid = 0;
    const int spawn_result =
        posix_spawn(&child_pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
      REXLOG_WARN("Restart requested, but launching a new process failed: {}",
                  std::strerror(spawn_result));
      return;
    }
#else
    REXLOG_WARN("Restart requested, but automatic restart is not implemented on this platform");
    return;
#endif

  #if REX_PLATFORM_MAC
    app_context().QuitFromUIThread();
  #else
    if (window()) {
      window()->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  #endif
  });
}

void Skate3BaseApp::SaveDrawFingerprintLog() {
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  REXLOG_WARN("Perf capture is unavailable because perf counters are disabled in this build");
  return;
#else
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream counters_filename;
  counters_filename << "perf_capture_counters_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
                    << ".csv";
  std::ostringstream filename;
  filename << "perf_capture_draw_fingerprints_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
           << ".csv";

  std::error_code ec;
  std::filesystem::create_directories(cache_root(), ec);
  const auto counters_path = cache_root() / counters_filename.str();
  const auto path = cache_root() / filename.str();
  if (rex::perf::StartCapture(counters_path, path)) {
    REXLOG_INFO("Started perf capture; counter log will be saved to {}",
                counters_path.string());
    REXLOG_INFO("Started perf capture; draw fingerprint log will be saved to {}",
                path.string());
  } else {
    REXLOG_WARN("Perf capture is already running");
  }
#endif
}

void Skate3BaseApp::LogUserMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("USER LOG MARKER #{}: F10 pressed", marker);
}

void Skate3BaseApp::LogDebugMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  rex::cvar::SetFlagByName("vulkan_debug_log_frame_summaries_remaining", "36000");
  rex::cvar::SetFlagByName("vulkan_debug_frame_summary_interval_frames", "300");
  rex::cvar::SetFlagByName("vulkan_debug_log_resolve_decisions_remaining", "10000");
  rex::cvar::SetFlagByName("vulkan_debug_log_team_profile_background_candidates_remaining",
                           "1000");
  rex::cvar::SetFlagByName("vulkan_debug_log_team_profile_background_bindings_remaining",
                           "256");
  rex::cvar::SetFlagByName("filesystem_debug_log_fe_asset_ops_remaining", "10000");
  rex::cvar::SetFlagByName("filesystem_debug_log_team_profile_background_remaining", "200");
  REXLOG_WARN(
      "USER DEBUG MARKER #{}: F9 pressed; enabled Vulkan frame summaries, resolve diagnostics, "
      "FE asset diagnostics, and team_profile_background_0 tracing",
      marker);
}

void Skate3BaseApp::ApplySelectedProfileToRuntime() {
  if (!runtime() || !runtime()->kernel_state() || !runtime()->kernel_state()->user_profile()) {
    return;
  }
  auto profiles = skate3::LoadProfiles(profiles_path_);
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    runtime()->kernel_state()->user_profile()->SetIdentity(profile->xuid, profile->gamertag);
  }
}

bool Skate3BaseApp::IsRecipeNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::set<std::string> Skate3BaseApp::DiscoverRecipeAliases(
    const std::filesystem::path& content_root) {
  static constexpr size_t kScanBytes = 8 * 1024 * 1024;
  static constexpr std::string_view kPrefix = "data/content/recipe/";

  std::set<std::string> aliases;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }

    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      continue;
    }

    std::string buffer;
    buffer.resize(kScanBytes);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(stream.gcount()));

    size_t pos = 0;
    while ((pos = buffer.find(kPrefix, pos)) != std::string::npos) {
      size_t name_start = pos + kPrefix.size();
      size_t name_end = name_start;
      while (name_end < buffer.size() && IsRecipeNameChar(buffer[name_end])) {
        ++name_end;
      }
      if (name_end > name_start) {
        aliases.insert(buffer.substr(name_start, name_end - name_start));
      }
      pos = name_end;
    }
  }

  return aliases;
}

bool Skate3BaseApp::CreateOverlayDirectory(
    const std::filesystem::path& overlay_root, std::string_view guest_path) {
  std::filesystem::path path = overlay_root;
  size_t segment_start = 0;
  while (segment_start < guest_path.size()) {
    const size_t slash = guest_path.find('/', segment_start);
    const size_t segment_end = slash == std::string_view::npos ? guest_path.size() : slash;
    if (segment_end > segment_start) {
      path /= std::string(guest_path.substr(segment_start, segment_end - segment_start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

void Skate3BaseApp::InstallRecipeOverlay() {
  if (recipe_overlay_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  const auto content_root = runtime()->game_data_root() / "data" / "content";
  if (!std::filesystem::exists(content_root)) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; content root not found: {}",
                content_root.string());
    return;
  }

  const auto aliases = DiscoverRecipeAliases(content_root);
  if (aliases.empty()) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; no recipe aliases found in {}",
                content_root.string());
    return;
  }

  const auto overlay_root = cache_root() / "vfs_big_directory_aliases";
  std::error_code ec;
  std::filesystem::create_directories(overlay_root, ec);
  if (ec) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create {}: {}",
                overlay_root.string(), ec.message());
    return;
  }

  size_t created = 0;
  for (const auto& alias : aliases) {
    const std::string guest_path = std::string("data/content/recipe/") + alias;
    if (CreateOverlayDirectory(overlay_root, guest_path)) {
      ++created;
    }
  }

  static constexpr std::string_view kBigDirectoryAliases[] = {
      "data/scene",
      "data/scene/anim",
      "data/scene/trickguide",
      "data/livingworld/PluginDescriptor",
      "data/state/livingworldentities/pedestrian/plugin",
  };
  for (std::string_view alias : kBigDirectoryAliases) {
    if (CreateOverlayDirectory(overlay_root, alias)) {
      ++created;
    }
  }

  if (!created) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create alias directories in {}",
                overlay_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      "skate3bigdirs:", overlay_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to initialize host device for {}",
                overlay_root.string());
    return;
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\content\\recipe",
      "skate3bigdirs:\\data\\content\\recipe");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\scene",
      "skate3bigdirs:\\data\\scene");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\livingworld\\PluginDescriptor",
      "skate3bigdirs:\\data\\livingworld\\PluginDescriptor");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\state\\livingworldentities\\pedestrian\\plugin",
      "skate3bigdirs:\\data\\state\\livingworldentities\\pedestrian\\plugin");
  recipe_overlay_installed_ = true;
  REXLOG_INFO("Installed Skate 3 BIG-directory VFS overlay with {} aliases from {}",
              created, content_root.string());
}

void Skate3BaseApp::InstallBigDeviceAliases() {
  if (big_device_aliases_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  // Skate 3 uses these file-server style schemes for world content probes.
  // Route them to the normal title mount so existing disc files resolve.
  runtime()->file_system()->RegisterSymbolicLink(
      "big:", "\\Device\\Harddisk0\\Partition1");
  runtime()->file_system()->RegisterSymbolicLink(
      "dlcbig:", "\\Device\\Harddisk0\\Partition1");
  big_device_aliases_installed_ = true;
}
