// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

namespace VideoCommon {

inline constexpr u32 OpenGLPipelineCacheVersion = 53;
inline constexpr u32 VulkanPipelineCacheVersion = 57;

// The oldest cache versions whose stored layout (pipeline keys and shader environments) matches
// this build's. Every cached pipeline is translated again from its stored guest programs at boot,
// so a translation change only bumps the cache version above, and a file from a version in
// [compatible, current) is carried over instead of discarded. Raise these to the new version
// whenever the serialized environment or a pipeline key changes layout.
inline constexpr u32 OpenGLPipelineCacheCompatibleVersion = 53;
inline constexpr u32 VulkanPipelineCacheCompatibleVersion = 57;

// Official Eglantine (cache version 18 at c0cb3325) and this fork (a different version) both
// used <shader dir>/<title>/vulkan.bin and vulkan_pipelines.bin. Each build deletes a file
// with another version at boot, so alternating builds made every run start with no pipelines
// (MKW log: "Deleting old pipeline cache", "Total Pipeline Count: 0", then hundreds of
// pipelines built on the GPU thread). Version-suffixed names let the builds coexist.
[[nodiscard]] inline std::filesystem::path VersionedPipelineCachePath(
    const std::filesystem::path& directory, std::string_view stem, u32 version) {
    return directory / fmt::format("{}.fork-v{}.bin", stem, version);
}

/// True when the file starts with this magic number followed by this cache version.
[[nodiscard]] inline bool PipelineCacheHeaderMatches(const std::filesystem::path& file,
                                                     const std::array<char, 8>& magic,
                                                     u32 version) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    std::array<char, 8> file_magic{};
    u32 file_version{};
    stream.read(file_magic.data(), file_magic.size());
    stream.read(reinterpret_cast<char*>(&file_version), sizeof(file_version));
    return stream.good() && file_magic == magic && file_version == version;
}

/// Moves a legacy-named cache that this exact cache version wrote to its versioned name, so an
/// existing warm cache survives the rename. A file of any other version (another build's) is
/// never touched; neither is an existing versioned file.
inline bool AdoptLegacyPipelineCache(const std::filesystem::path& legacy,
                                     const std::filesystem::path& versioned,
                                     const std::array<char, 8>& magic, u32 version) {
    std::error_code error;
    if (std::filesystem::exists(versioned, error) || error) {
        return false;
    }
    if (!PipelineCacheHeaderMatches(legacy, magic, version)) {
        return false;
    }
    std::filesystem::rename(legacy, versioned, error);
    return !error;
}

/// The version in a "<stem>.fork-v<version>.bin" name, or 0 for any other name.
[[nodiscard]] inline u32 PipelineCacheFileVersion(std::string_view name, std::string_view stem) {
    const std::string prefix = fmt::format("{}.fork-v", stem);
    if (!name.starts_with(prefix) || !name.ends_with(".bin")) {
        return 0;
    }
    const std::string_view digits = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    u32 version = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9' || version > 100'000'000) {
            return 0;
        }
        version = version * 10 + static_cast<u32>(digit - '0');
    }
    return digits.empty() ? 0 : version;
}

/// Brings this title's "<stem>" cache up to `version` before it is loaded:
/// - Without a file for `version`, the newest file of a version in [compatible_version, version)
///   is renamed to it. With `versioned_header`, the file must hold that version after its 8-byte
///   magic number, and it is rewritten to `version`.
/// - Files of `stem` for older versions are then deleted, with their quarantined copies. Files of
///   newer versions (a newer build sharing the directory) and unversioned names are kept.
inline void UpgradePipelineCacheFile(const std::filesystem::path& directory,
                                     std::string_view stem, u32 version, u32 compatible_version,
                                     bool versioned_header) {
    std::error_code error;
    const auto current = VersionedPipelineCachePath(directory, stem, version);
    if (!std::filesystem::exists(current, error) && !error) {
        u32 newest = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            const u32 found =
                PipelineCacheFileVersion(entry.path().filename().string(), stem);
            if (found >= compatible_version && found < version && found > newest) {
                newest = found;
            }
        }
        if (newest != 0) {
            const auto source = VersionedPipelineCachePath(directory, stem, newest);
            bool adopt = true;
            if (versioned_header) {
                std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
                u32 file_version = 0;
                file.seekg(8).read(reinterpret_cast<char*>(&file_version), sizeof(file_version));
                adopt = file.good() && file_version == newest;
                if (adopt) {
                    file.seekp(8).write(reinterpret_cast<const char*>(&version), sizeof(version));
                    adopt = file.good();
                }
            }
            if (adopt) {
                std::filesystem::rename(source, current, error);
            }
        }
    }
    error.clear();
    std::vector<std::filesystem::path> stale;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::string name = entry.path().filename().string();
        if (const auto quarantine = name.find(".bin.invalid"); quarantine != std::string::npos) {
            name.resize(quarantine + 4);
        }
        const u32 found = PipelineCacheFileVersion(name, stem);
        if (found != 0 && found < version) {
            stale.push_back(entry.path());
        }
    }
    for (const auto& path : stale) {
        std::filesystem::remove(path, error);
    }
}

/// Copies `source` to a not-yet-existing `destination`, rewriting the version after its magic
/// number. Used to seed a new cache version from a compatible older file.
inline bool CopyCompatiblePipelineCache(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        const std::array<char, 8>& magic, u32 source_version,
                                        u32 destination_version) {
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error ||
        !PipelineCacheHeaderMatches(source, magic, source_version)) {
        return false;
    }
    auto temporary = destination;
    temporary += ".migrating";
    if (!std::filesystem::copy_file(source, temporary,
                                    std::filesystem::copy_options::overwrite_existing, error)) {
        return false;
    }
    {
        std::fstream stream(temporary, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(magic.size()));
        stream.write(reinterpret_cast<const char*>(&destination_version),
                     sizeof(destination_version));
        stream.close();
        if (stream.fail()) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace VideoCommon
