// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "video_core/pipeline_cache_bundle.h"
#include "video_core/pipeline_cache_files.h"

namespace {

namespace fs = std::filesystem;

struct TempDir {
    TempDir() {
        path = fs::temp_directory_path() /
               ("pipeline_cache_upgrade_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

void WriteCache(const fs::path& file, u32 version, char payload) {
    std::ofstream stream(file, std::ios::binary);
    const std::array<char, 8> magic{'t', 'e', 's', 't', 'c', 'a', 'c', 'h'};
    stream.write(magic.data(), magic.size());
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.put(payload);
}

std::pair<u32, char> ReadCache(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::array<char, 8> magic{};
    u32 version{};
    stream.read(magic.data(), magic.size());
    stream.read(reinterpret_cast<char*>(&version), sizeof(version));
    return {version, static_cast<char>(stream.get())};
}

} // Anonymous namespace

TEST_CASE("Pipeline cache version parsing", "[video_core]") {
    using VideoCommon::PipelineCacheFileVersion;
    CHECK(PipelineCacheFileVersion("vulkan.fork-v56.bin", "vulkan") == 56);
    CHECK(PipelineCacheFileVersion("vulkan_used.fork-v56.bin", "vulkan") == 0);
    CHECK(PipelineCacheFileVersion("vulkan.bin", "vulkan") == 0);
    CHECK(PipelineCacheFileVersion("vulkan.fork-v.bin", "vulkan") == 0);
    CHECK(PipelineCacheFileVersion("vulkan.fork-v5x.bin", "vulkan") == 0);
}

TEST_CASE("Pipeline caches carry over from a compatible version", "[video_core]") {
    TempDir dir;
    const auto path = [&](u32 version) {
        return VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan", version);
    };
    WriteCache(path(50), 50, 'a');
    WriteCache(path(52), 52, 'b');
    WriteCache(path(53), 53, 'c');
    WriteCache(path(57), 57, 'd');
    WriteCache(dir.path / "vulkan.fork-v52.bin.invalid", 52, 'e');
    WriteCache(dir.path / "vulkan.bin", 18, 'f');
    WriteCache(VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan_used", 53), 0, 'g');

    VideoCommon::UpgradePipelineCacheFile(dir.path, "vulkan", 56, 52, true);

    REQUIRE(fs::exists(path(56)));
    CHECK(ReadCache(path(56)) == std::pair<u32, char>{56, 'c'});
    CHECK(!fs::exists(path(50)));
    CHECK(!fs::exists(path(52)));
    CHECK(!fs::exists(path(53)));
    CHECK(!fs::exists(dir.path / "vulkan.fork-v52.bin.invalid"));
    CHECK(fs::exists(path(57)));
    CHECK(fs::exists(dir.path / "vulkan.bin"));
    CHECK(fs::exists(VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan_used", 53)));
}

TEST_CASE("Pipeline caches below the compatible version are dropped", "[video_core]") {
    TempDir dir;
    const auto path = [&](u32 version) {
        return VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan", version);
    };
    WriteCache(path(50), 50, 'a');
    VideoCommon::UpgradePipelineCacheFile(dir.path, "vulkan", 56, 52, true);
    CHECK(!fs::exists(path(50)));
    CHECK(!fs::exists(path(56)));

    // A file whose header disagrees with its name is not adopted.
    WriteCache(path(54), 51, 'b');
    VideoCommon::UpgradePipelineCacheFile(dir.path, "vulkan", 56, 52, true);
    CHECK(!fs::exists(path(56)));
    CHECK(!fs::exists(path(54)));
}

TEST_CASE("An existing current pipeline cache is kept as it is", "[video_core]") {
    TempDir dir;
    const auto path = [&](u32 version) {
        return VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan_used", version);
    };
    WriteCache(path(55), 0, 'a');
    WriteCache(path(56), 0, 'b');
    VideoCommon::UpgradePipelineCacheFile(dir.path, "vulkan_used", 56, 52, false);
    CHECK(ReadCache(path(56)).second == 'b');
    CHECK(!fs::exists(path(55)));
}

TEST_CASE("Pipeline cache bundles move a cache to another shader directory", "[video_core]") {
    TempDir source;
    TempDir target;
    constexpr u64 title = 0x0400486024DEC000ULL;
    const auto title_dir = [&](const TempDir& dir) {
        return dir.path / fmt::format("{:016x}", title);
    };
    fs::create_directories(title_dir(source));
    constexpr u32 compatible = VideoCommon::VulkanPipelineCacheCompatibleVersion;
    constexpr u32 current = VideoCommon::VulkanPipelineCacheVersion;
    WriteCache(VideoCommon::VersionedPipelineCachePath(title_dir(source), "vulkan", compatible),
               compatible, 'v');
    WriteCache(VideoCommon::VersionedPipelineCachePath(title_dir(source), "vulkan_pipelines",
                                                       current),
               current, 'd');
    const fs::path bundle = source.path / "cache.elysium-shaders";

    const auto exported = VideoCommon::ExportPipelineCacheBundle(source.path, title, bundle);
    REQUIRE(exported.ok);
    CHECK(exported.files == 1); // The driver's cache stays behind.

    CHECK(!VideoCommon::ImportPipelineCacheBundle(target.path, bundle, title + 1).ok);
    const auto imported = VideoCommon::ImportPipelineCacheBundle(target.path, bundle, title);
    REQUIRE(imported.ok);
    CHECK(imported.title_id == title);
    CHECK(imported.files == 1);
    const auto installed =
        VideoCommon::VersionedPipelineCachePath(title_dir(target), "vulkan", current);
    REQUIRE(fs::exists(installed));
    CHECK(ReadCache(installed) == std::pair<u32, char>{current, 'v'});

    // The same cache again: the local copy is as large, so it is kept.
    const auto again = VideoCommon::ImportPipelineCacheBundle(target.path, bundle, 0);
    REQUIRE(again.ok);
    CHECK(again.files == 0);
    CHECK(again.skipped == 1);
}
