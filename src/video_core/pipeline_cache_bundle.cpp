// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "common/zstd_compression.h"
#include "video_core/pipeline_cache_bundle.h"
#include "video_core/pipeline_cache_files.h"

namespace VideoCommon {
namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 8> BundleMagic{'E', 'L', 'Y', 'S', 'S', 'H', 'D', 'R'};
constexpr u32 BundleFormat = 1;
constexpr u64 MaxBundleFileBytes = 4ULL << 30;

struct CacheStem {
    std::string_view name;
    u32 version;
    u32 compatible_version;
    bool versioned_header;
};

constexpr std::array<CacheStem, 3> Stems{{
    {"vulkan", VulkanPipelineCacheVersion, VulkanPipelineCacheCompatibleVersion, true},
    {"vulkan_used", VulkanPipelineCacheVersion, VulkanPipelineCacheCompatibleVersion, false},
    {"opengl", OpenGLPipelineCacheVersion, OpenGLPipelineCacheCompatibleVersion, true},
}};

std::optional<std::pair<const CacheStem*, u32>> ParseName(std::string_view name) {
    for (const CacheStem& stem : Stems) {
        if (const u32 version = PipelineCacheFileVersion(name, stem.name); version != 0) {
            return std::pair{&stem, version};
        }
    }
    return std::nullopt;
}

fs::path TitleDirectory(const fs::path& shader_dir, u64 title_id) {
    return shader_dir / fmt::format("{:016x}", title_id);
}

template <typename T>
void Write(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool Read(std::ifstream& file, T& value) {
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

std::optional<std::vector<u8>> ReadWhole(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

} // Anonymous namespace

PipelineCacheBundleResult ExportPipelineCacheBundle(const fs::path& shader_dir, u64 title_id,
                                                    const fs::path& bundle) {
    PipelineCacheBundleResult result{.title_id = title_id};
    const fs::path directory = TitleDirectory(shader_dir, title_id);
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        result.error = "This game has no pipeline cache yet.";
        return result;
    }
    for (const CacheStem& stem : Stems) {
        UpgradePipelineCacheFile(directory, stem.name, stem.version, stem.compatible_version,
                                 stem.versioned_header);
    }
    std::vector<std::pair<std::string, std::vector<u8>>> files;
    for (const CacheStem& stem : Stems) {
        const fs::path path = VersionedPipelineCachePath(directory, stem.name, stem.version);
        if (!fs::exists(path, error)) {
            continue;
        }
        auto data = ReadWhole(path);
        if (!data || data->empty() || data->size() > MaxBundleFileBytes) {
            continue;
        }
        files.emplace_back(path.filename().string(), std::move(*data));
    }
    if (files.empty()) {
        result.error = "This game has no pipeline cache yet.";
        return result;
    }
    std::ofstream file(bundle, std::ios::binary | std::ios::trunc);
    if (!file) {
        result.error = "The bundle file could not be created.";
        return result;
    }
    file.write(BundleMagic.data(), BundleMagic.size());
    Write(file, BundleFormat);
    Write(file, title_id);
    Write(file, static_cast<u32>(files.size()));
    for (const auto& [name, data] : files) {
        const std::vector<u8> compressed = Common::Compression::CompressDataZSTDDefault(
            data.data(), data.size());
        Write(file, static_cast<u16>(name.size()));
        file.write(name.data(), name.size());
        Write(file, static_cast<u64>(data.size()));
        Write(file, static_cast<u64>(compressed.size()));
        file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    }
    if (!file.flush()) {
        result.error = "The bundle file could not be written.";
        return result;
    }
    result.ok = true;
    result.files = files.size();
    return result;
}

PipelineCacheBundleResult ImportPipelineCacheBundle(const fs::path& shader_dir,
                                                    const fs::path& bundle,
                                                    u64 expected_title_id) {
    PipelineCacheBundleResult result;
    std::ifstream file(bundle, std::ios::binary);
    std::array<char, 8> magic{};
    u32 format{};
    u32 count{};
    if (!file || !file.read(magic.data(), magic.size()) || magic != BundleMagic ||
        !Read(file, format) || !Read(file, result.title_id) || !Read(file, count)) {
        result.error = "This is not a pipeline cache bundle.";
        return result;
    }
    if (format != BundleFormat) {
        result.error = "This bundle was made by a newer build.";
        return result;
    }
    if (expected_title_id != 0 && result.title_id != expected_title_id) {
        result.error = fmt::format("This bundle belongs to another game ({:016X}).",
                                   result.title_id);
        return result;
    }
    const fs::path directory = TitleDirectory(shader_dir, result.title_id);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        result.error = "The pipeline cache directory could not be created.";
        return result;
    }
    for (u32 index = 0; index < count && index < 16; ++index) {
        u16 name_size{};
        u64 raw_size{};
        u64 compressed_size{};
        std::string name;
        if (!Read(file, name_size) || name_size == 0 || name_size > 128) {
            result.error = "The bundle is damaged.";
            return result;
        }
        name.resize(name_size);
        if (!file.read(name.data(), name_size) || !Read(file, raw_size) ||
            !Read(file, compressed_size) || raw_size > MaxBundleFileBytes ||
            compressed_size > MaxBundleFileBytes) {
            result.error = "The bundle is damaged.";
            return result;
        }
        std::vector<u8> compressed(compressed_size);
        if (!file.read(reinterpret_cast<char*>(compressed.data()), compressed_size)) {
            result.error = "The bundle is damaged.";
            return result;
        }
        const auto parsed = ParseName(name);
        if (!parsed || parsed->second < parsed->first->compatible_version) {
            ++result.skipped; // An unknown file, or a layout this build cannot read.
            continue;
        }
        const auto [stem, version] = *parsed;
        auto data = Common::Compression::TryDecompressDataZSTD(compressed, raw_size);
        if (!data || data->size() != raw_size) {
            result.error = "The bundle is damaged.";
            return result;
        }
        u32 target_version = version;
        if (version < stem->version) {
            target_version = stem->version;
            if (stem->versioned_header) {
                u32 header_version{};
                if (data->size() < 12) {
                    ++result.skipped;
                    continue;
                }
                std::memcpy(&header_version, data->data() + 8, sizeof(header_version));
                if (header_version != version) {
                    ++result.skipped;
                    continue;
                }
                std::memcpy(data->data() + 8, &target_version, sizeof(target_version));
            }
        }
        const fs::path target = VersionedPipelineCachePath(directory, stem->name, target_version);
        if (fs::exists(target, error) && fs::file_size(target, error) >= data->size()) {
            ++result.skipped;
            continue;
        }
        const fs::path temporary = fs::path{target} += ".import";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data->data()), data->size());
            if (!out.flush()) {
                result.error = "The pipeline cache could not be written.";
                return result;
            }
        }
        fs::rename(temporary, target, error);
        if (error) {
            fs::remove(temporary, error);
            result.error = "The pipeline cache could not be written.";
            return result;
        }
        ++result.files;
    }
    result.ok = true;
    return result;
}

} // namespace VideoCommon
