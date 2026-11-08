// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"
#include "video_core/shader_environment.h"

namespace {
constexpr std::array<char, 8> CacheMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr u32 Version = 57;
constexpr size_t ComputeKeySize = 64;
constexpr size_t GraphicsKeySize = 256;

std::filesystem::path TempDir(std::string_view name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("elysium-merge-test-" + std::string(name));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteCache(const std::filesystem::path& path, std::array<char, 8> magic, u32 version,
                std::span<const char> body = {}) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(magic.data(), magic.size()).write(reinterpret_cast<const char*>(&version),
                                              sizeof(version));
    if (!body.empty()) {
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
}

auto Merge(const std::filesystem::path& out, std::span<const std::filesystem::path> in) {
    return VideoCommon::MergePipelineCaches(out, in, Version, ComputeKeySize, GraphicsKeySize);
}
} // Anonymous namespace

TEST_CASE("Merging rejects caches this build would not load", "[video_core][pipeline_cache]") {
    const auto dir = TempDir("reject");
    const auto good = dir / "good.bin", bad_magic = dir / "magic.bin";
    const auto bad_version = dir / "version.bin", missing = dir / "missing.bin";
    WriteCache(good, CacheMagic, Version);
    WriteCache(bad_magic, {'n', 'o', 't', 'a', 'c', 'a', 'c', 'h'}, Version);
    WriteCache(bad_version, CacheMagic, Version + 1);

    const std::vector<std::filesystem::path> inputs{good, bad_magic, bad_version, missing};
    const auto result = Merge(dir / "out.bin", inputs);
    CHECK(result.inputs_read == 1);
    CHECK(result.rejected_files == 3);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Merging nothing usable leaves no output", "[video_core][pipeline_cache]") {
    const auto dir = TempDir("nothing");
    const auto bad = dir / "bad.bin", out = dir / "out.bin";
    WriteCache(bad, {'n', 'o', 't', 'a', 'c', 'a', 'c', 'h'}, Version);

    const std::vector<std::filesystem::path> inputs{bad};
    const auto result = Merge(out, inputs);
    CHECK(result.inputs_read == 0);
    // Better no file than an empty one standing where a cache used to be.
    CHECK_FALSE(std::filesystem::exists(out));
    std::filesystem::remove_all(dir);
}

TEST_CASE("A header-only cache merges to a header-only cache", "[video_core][pipeline_cache]") {
    const auto dir = TempDir("header");
    const auto good = dir / "good.bin", out = dir / "out.bin";
    WriteCache(good, CacheMagic, Version);

    const std::vector<std::filesystem::path> inputs{good};
    const auto result = Merge(out, inputs);
    CHECK(result.inputs_read == 1);
    CHECK(result.records_written == 0);
    REQUIRE(std::filesystem::exists(out));
    CHECK(std::filesystem::file_size(out) == CacheMagic.size() + sizeof(Version));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Merging onto an input is refused", "[video_core][pipeline_cache]") {
    const auto dir = TempDir("self");
    const auto cache = dir / "cache.bin";
    WriteCache(cache, CacheMagic, Version);
    const auto before = std::filesystem::file_size(cache);

    const std::vector<std::filesystem::path> inputs{cache};
    const auto result = Merge(cache, inputs);
    // The output is written last, so without this check the input would be read and then
    // destroyed by its own result.
    CHECK(result.inputs_read == 0);
    CHECK(result.rejected_files == 1);
    CHECK(std::filesystem::file_size(cache) == before);
    std::filesystem::remove_all(dir);
}

// The reader truncates a file it finds a bad record in. Merging must never hand it an original:
// doing so once cut a 163 MB cache down to 9 KB.
TEST_CASE("Merging never alters its inputs, even invalid ones",
          "[video_core][pipeline_cache]") {
    const auto dir = TempDir("immutable");
    const auto cache = dir / "cache.bin", out = dir / "out.bin";
    // A valid header followed by a record count the reader rejects.
    const std::array<char, 4> garbage{'\x7f', '\x7f', '\x7f', '\x7f'};
    WriteCache(cache, CacheMagic, Version, garbage);
    const auto before_size = std::filesystem::file_size(cache);
    REQUIRE(before_size == CacheMagic.size() + sizeof(Version) + garbage.size());

    const std::vector<std::filesystem::path> inputs{cache};
    (void)Merge(out, inputs);

    CHECK(std::filesystem::file_size(cache) == before_size);
    CHECK_FALSE(std::filesystem::exists(cache.string() + ".invalid"));
    std::filesystem::remove_all(dir);
}
