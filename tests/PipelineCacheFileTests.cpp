#pragma once
// The on-disk pipeline cache container - every damage class the
// concurrent-instances race can produce must be rejected BEFORE the driver
// sees the blob, and a save must never tear the file.

#include "Platform/Vulkan/RHI/PipelineCacheFile.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

namespace PipelineCacheFileTestsDetail {

	using namespace TinyEngine::rhi;

	inline PipelineCacheIdentity MakeIdentity()
	{
		PipelineCacheIdentity id;
		id.VendorId = 0x10de;
		id.DeviceId = 0x2208;
		for (size_t i = 0; i < kPipelineCacheUuidSize; i++)
			id.CacheUuid[i] = (uint8_t)(0xA0 + i);
		return id;
	}

	// A well-formed driver blob: the spec's 32-byte header + a body.
	inline std::vector<uint8_t> MakePayload(const PipelineCacheIdentity& id, size_t bodySize, uint8_t fill)
	{
		std::vector<uint8_t> payload(32 + bodySize, fill);
		const uint32_t fields[4] = { 32u, 1u, id.VendorId, id.DeviceId };
		memcpy(payload.data(), fields, sizeof(fields));
		memcpy(payload.data() + 16, id.CacheUuid, kPipelineCacheUuidSize);
		return payload;
	}

	inline PipelineCacheFileStatus Parse(const std::vector<uint8_t>& file, const PipelineCacheIdentity& id)
	{
		const uint8_t* payload = nullptr;
		size_t size = 0;
		return ParsePipelineCacheFile(file, id, payload, size);
	}

	inline std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}
}

TEST_CASE("PipelineCacheFile: a built file round-trips its payload")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();
	const std::vector<uint8_t> payload = MakePayload(id, 4096, 0x5A);
	const std::vector<uint8_t> file = BuildPipelineCacheFile(payload.data(), payload.size(), id);

	const uint8_t* parsed = nullptr;
	size_t parsedSize = 0;
	CHECK(ParsePipelineCacheFile(file, id, parsed, parsedSize) == PipelineCacheFileStatus::Ok);
	REQUIRE(parsedSize == payload.size());
	CHECK(memcmp(parsed, payload.data(), parsedSize) == 0);
}

TEST_CASE("PipelineCacheFile: every truncation is rejected")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();
	const std::vector<uint8_t> payload = MakePayload(id, 512, 0x11);
	const std::vector<uint8_t> file = BuildPipelineCacheFile(payload.data(), payload.size(), id);

	for (size_t size = 0; size < file.size(); size++)
	{
		const std::vector<uint8_t> torn(file.begin(), file.begin() + (std::ptrdiff_t)size);
		CHECK(Parse(torn, id) != PipelineCacheFileStatus::Ok);
	}
}

TEST_CASE("PipelineCacheFile: a same-size spliced file is rejected")
{
	// The in-place truncating writer's failure shape: a reader sees the old
	// file's front + the new file's tail. Every header field stays plausible;
	// only the hash catches it.
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();
	const std::vector<uint8_t> a = MakePayload(id, 4096, 0x11);
	const std::vector<uint8_t> b = MakePayload(id, 4096, 0x22);
	const std::vector<uint8_t> fileA = BuildPipelineCacheFile(a.data(), a.size(), id);
	const std::vector<uint8_t> fileB = BuildPipelineCacheFile(b.data(), b.size(), id);

	std::vector<uint8_t> spliced(fileA.begin(), fileA.begin() + 2048);
	spliced.insert(spliced.end(), fileB.begin() + 2048, fileB.end());
	REQUIRE(spliced.size() == fileA.size());
	CHECK(Parse(spliced, id) == PipelineCacheFileStatus::HashMismatch);
}

TEST_CASE("PipelineCacheFile: every single-byte payload flip is rejected")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();
	const std::vector<uint8_t> payload = MakePayload(id, 256, 0x33);
	const std::vector<uint8_t> file = BuildPipelineCacheFile(payload.data(), payload.size(), id);

	for (size_t offset = 72; offset < file.size(); offset++)
	{
		std::vector<uint8_t> damaged = file;
		damaged[offset] ^= 0x40;
		CHECK(Parse(damaged, id) == PipelineCacheFileStatus::HashMismatch);
	}
}

TEST_CASE("PipelineCacheFile: the old UUID-prefixed format is not this format")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();
	const std::vector<uint8_t> payload = MakePayload(id, 1024, 0x44);
	std::vector<uint8_t> legacy(id.CacheUuid, id.CacheUuid + kPipelineCacheUuidSize);
	legacy.insert(legacy.end(), payload.begin(), payload.end());
	CHECK(Parse(legacy, id) == PipelineCacheFileStatus::BadMagic);
}

TEST_CASE("PipelineCacheFile: another GPU/driver/display mode's file is a mismatch, not damage")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity writer = MakeIdentity();
	const std::vector<uint8_t> payload = MakePayload(writer, 64, 0x55);
	const std::vector<uint8_t> file = BuildPipelineCacheFile(payload.data(), payload.size(), writer);

	// NVIDIA's pipelineCacheUUID differs between a display-server and a
	// headless run of the same binary.
	PipelineCacheIdentity reader = writer;
	reader.CacheUuid[15] ^= 0xFF;
	CHECK(Parse(file, reader) == PipelineCacheFileStatus::IdentityMismatch);
	reader = writer;
	reader.DeviceId++;
	CHECK(Parse(file, reader) == PipelineCacheFileStatus::IdentityMismatch);
}

TEST_CASE("PipelineCacheFile: an unusable driver header is rejected even with a good hash")
{
	using namespace PipelineCacheFileTestsDetail;
	const PipelineCacheIdentity id = MakeIdentity();

	const auto rebuiltWith = [&](auto&& damage)
	{
		std::vector<uint8_t> payload = MakePayload(id, 64, 0x66);
		damage(payload);
		return Parse(BuildPipelineCacheFile(payload.data(), payload.size(), id), id);
	};

	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p.resize(31); }) == PipelineCacheFileStatus::BadDriverHeader);
	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p[0] = 16; }) == PipelineCacheFileStatus::BadDriverHeader);
	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p[0] = 0xFF; p[1] = 0xFF; }) == PipelineCacheFileStatus::BadDriverHeader);
	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p[4] = 2; }) == PipelineCacheFileStatus::BadDriverHeader);
	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p[8] ^= 1; }) == PipelineCacheFileStatus::BadDriverHeader);
	CHECK(rebuiltWith([](std::vector<uint8_t>& p) { p[16] ^= 1; }) == PipelineCacheFileStatus::BadDriverHeader);
}

TEST_CASE("PipelineCacheFile: concurrent atomic saves never expose a torn file")
{
	using namespace PipelineCacheFileTestsDetail;
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec) / ("te_pipeline_cache_test_" + std::to_string((unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id())));
	fs::create_directories(dir, ec);
	const fs::path path = dir / "pipeline.bin";

	const PipelineCacheIdentity id = MakeIdentity();
	std::atomic<int> writersRunning{ 4 };
	std::vector<std::thread> writers;
	for (uint8_t writer = 0; writer < 4; writer++)
	{
		writers.emplace_back([&, writer]
		{
			// Different sizes per writer: a non-atomic save would splice them.
			const std::vector<uint8_t> payload = MakePayload(id, 64 * 1024 * (size_t)(writer + 1), (uint8_t)(0x70 + writer));
			const std::vector<uint8_t> file = BuildPipelineCacheFile(payload.data(), payload.size(), id);
			for (int i = 0; i < 200; i++)
				WritePipelineCacheFileAtomic(path, file);
			writersRunning.fetch_sub(1);
		});
	}

	size_t readsWithFile = 0;
	size_t damaged = 0;
	while (writersRunning.load() > 0)
	{
		const std::vector<uint8_t> file = ReadAll(path);
		if (file.empty())
			continue; // before the first rename landed
		readsWithFile++;
		if (Parse(file, id) != PipelineCacheFileStatus::Ok)
			damaged++;
	}
	for (std::thread& writer : writers)
		writer.join();

	CHECK(readsWithFile > 0);
	CHECK(damaged == 0);
	CHECK(Parse(ReadAll(path), id) == PipelineCacheFileStatus::Ok);

	// Every temp file was renamed away.
	size_t leftovers = 0;
	for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
		if (entry.path().filename() != "pipeline.bin")
			leftovers++;
	CHECK(leftovers == 0);
	fs::remove_all(dir, ec);
}

TEST_CASE("PipelineCacheFile: a failed save leaves the destination untouched")
{
	using namespace PipelineCacheFileTestsDetail;
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec) / ("te_pipeline_cache_fail_test_" + std::to_string((unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id())));
	fs::create_directories(dir, ec);

	// A directory at the destination path makes the rename fail.
	const fs::path path = dir / "pipeline.bin";
	fs::create_directories(path / "child", ec);
	const std::vector<uint8_t> bytes(128, 0x7E);
	CHECK_FALSE(WritePipelineCacheFileAtomic(path, bytes));
	CHECK(fs::is_directory(path, ec));

	size_t entries = 0;
	for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
	{
		(void)entry;
		entries++;
	}
	CHECK(entries == 1); // the temp file is gone
	fs::remove_all(dir, ec);
}
