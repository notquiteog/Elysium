// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"

#include <sl_security.h>

#include <softpub.h>
#include <wintrust.h>

namespace Vulkan::Streamline {
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct State {
    HMODULE module{};
    Api api{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    u32 extra_graphics_queues{};
    u32 extra_compute_queues{};
    // Loaded at start; cleared when the device can't run them.
    bool dlss{};
    bool neural_rendering{};
    bool frame_gen{};
    bool started{};
    bool attached{};
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    if (!IsSignedByNvidia(target)) {
        LOG_WARNING(Render_Vulkan, "Cached DLSS {} is not signed by NVIDIA",
                    Common::FS::PathToUTF8String(target));
        return {};
    }
    return dir;
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info) &&
        Resolve(m, "slSetTagForFrame", api.set_tag_for_frame)) {
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

void AddNames(std::vector<std::string>& list, uint32_t count, const char** names) {
    for (uint32_t i = 0; i < count; ++i) {
        if (std::ranges::find(list, names[i]) == list.end()) {
            list.emplace_back(names[i]);
        }
    }
}

bool WantsFrameGen() {
    return Settings::values.frame_gen.GetValue() &&
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Dlss;
}

const char* FeatureName(sl::Feature feature) {
    switch (feature) {
    case sl::kFeatureDLSS:
        return "DLSS";
    case sl::kFeatureDLSS_NR:
        return "DLSS 5 neural rendering";
    case sl::kFeatureDLSS_G:
        return "DLSS frame generation";
    case sl::kFeatureReflex:
        return "Reflex";
    case sl::kFeaturePCL:
        return "PC latency";
    default:
        return "feature";
    }
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    const bool want_dlss = Settings::values.nvidia_dlss.GetValue();
    const bool want_neural = Settings::values.nvidia_dlss_nr.GetValue();
    const bool want_frame_gen = WantsFrameGen();
    if (!want_dlss && !want_neural && !want_frame_gen) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (!files.model.empty() && !IsSignedByNvidia(files.model)) {
            LOG_WARNING(Render_Vulkan, "DLSS model {} is not signed by NVIDIA; using FSR 3.1",
                        Common::FS::PathToUTF8String(files.model));
            return false;
        }
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); using FSR 3.1",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty() && want_dlss) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Frame generation needs Reflex, and PCL for its markers.
    std::vector<sl::Feature> features;
    if (want_dlss) {
        features.push_back(sl::kFeatureDLSS);
    }
    if (want_neural) {
        features.push_back(sl::kFeatureDLSS_NR);
    }
    if (want_frame_gen) {
        features.insert(features.end(), {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL});
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const Api& api = state.api;
    if (const sl::Result result = api.init(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); using FSR 3.1",
                  static_cast<int>(result));
        return false;
    }

    state.instance_extensions.clear();
    state.device_extensions.clear();
    state.extra_graphics_queues = 0;
    state.extra_compute_queues = 0;
    const auto require = [&](sl::Feature feature) {
        sl::FeatureRequirements requirements{};
        if (const sl::Result result = api.get_feature_requirements(feature, requirements);
            result != sl::Result::eOk) {
            LOG_WARNING(Render_Vulkan,
                        "Streamline found no {} plugin (result {}); put its sl.*.dll next to "
                        "sl.interposer.dll",
                        FeatureName(feature), static_cast<int>(result));
            return false;
        }
        AddNames(state.instance_extensions, requirements.vkNumInstanceExtensions,
                 requirements.vkInstanceExtensions);
        AddNames(state.device_extensions, requirements.vkNumDeviceExtensions,
                 requirements.vkDeviceExtensions);
        state.extra_graphics_queues += requirements.vkNumGraphicsQueuesRequired;
        state.extra_compute_queues += requirements.vkNumComputeQueuesRequired;
        for (uint32_t i = 0; i < requirements.vkNumFeatures12; ++i) {
            LOG_INFO(Render_Vulkan, "{} wants the Vulkan 1.2 feature {}", FeatureName(feature),
                     requirements.vkFeatures12[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumFeatures13; ++i) {
            LOG_INFO(Render_Vulkan, "{} wants the Vulkan 1.3 feature {}", FeatureName(feature),
                     requirements.vkFeatures13[i]);
        }
        // Without a native optical flow queue DLSS-G runs its optical flow through interop.
        if (requirements.vkNumOpticalFlowQueuesRequired != 0) {
            LOG_INFO(Render_Vulkan, "{} would like {} optical flow queue(s); using interop",
                     FeatureName(feature), requirements.vkNumOpticalFlowQueuesRequired);
        }
        return true;
    };
    state.dlss = want_dlss && require(sl::kFeatureDLSS);
    state.neural_rendering = want_neural && require(sl::kFeatureDLSS_NR);
    state.frame_gen = want_frame_gen && require(sl::kFeatureDLSS_G) &&
                      require(sl::kFeatureReflex) && require(sl::kFeaturePCL);
    if (!state.dlss && !state.neural_rendering && !state.frame_gen) {
        api.shutdown();
        return false;
    }
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    for (const sl::Feature feature : features) {
        sl::FeatureVersion version{};
        if (api.get_feature_version(feature, version) == sl::Result::eOk) {
            LOG_INFO(Render_Vulkan, "NVIDIA Streamline {}: {} (NGX {})", version.versionSL.toStr(),
                     FeatureName(feature), version.versionNGX.toStr());
        }
    }
    LOG_INFO(Render_Vulkan,
             "NVIDIA Streamline started from {} ({} extra graphics, {} compute queues)",
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             state.extra_graphics_queues, state.extra_compute_queues);
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

u32 ExtraQueues() {
    const State& state = GetState();
    return state.started ? state.extra_graphics_queues + state.extra_compute_queues : 0;
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 extra_queues, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    if (extra_queues < state.extra_graphics_queues + state.extra_compute_queues) {
        LOG_WARNING(Render_Vulkan,
                    "The GPU's graphics queue family is too small for DLSS frame generation");
        state.frame_gen = false;
    }
    // Streamline's queues follow the renderer's one in the graphics family, graphics first.
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.graphicsQueueFamily = graphics_family;
    info.graphicsQueueIndex = 1;
    info.computeQueueFamily = graphics_family;
    info.computeQueueIndex = 1 + state.extra_graphics_queues;
    if (const sl::Result result = state.api.set_vulkan_info(info); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    const auto supported = [&](bool& loaded, sl::Feature feature) {
        if (!loaded) {
            return;
        }
        if (const sl::Result result = state.api.is_feature_supported(feature, adapter);
            result != sl::Result::eOk) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA {} is not supported on this GPU or driver (Streamline result {})",
                        FeatureName(feature), static_cast<int>(result));
            loaded = false;
        }
    };
    supported(state.dlss, sl::kFeatureDLSS);
    supported(state.neural_rendering, sl::kFeatureDLSS_NR);
    supported(state.frame_gen, sl::kFeatureDLSS_G);
    supported(state.frame_gen, sl::kFeatureReflex);
    supported(state.frame_gen, sl::kFeaturePCL);
    if (!state.dlss && !state.neural_rendering && !state.frame_gen) {
        Stop();
        return false;
    }
    // Manual hooking: Streamline does its per-frame work in these.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
    hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    state.attached = true;
    LOG_INFO(Render_Vulkan,
             "NVIDIA Streamline attached: DLSS {}, DLSS 5 neural rendering {}, DLSS frame "
             "generation {}",
             state.dlss ? "on" : "off", state.neural_rendering ? "on" : "off",
             state.frame_gen ? "on" : "off");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.dlss;
}

bool IsNeuralRenderingAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.neural_rendering;
}

bool IsFrameGenAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.frame_gen;
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    const sl::Result result = state.api.shutdown();
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

const Api& GetApi() {
    return GetState().api;
}

} // namespace Vulkan::Streamline
