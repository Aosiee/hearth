#include "hearth/rhi/vulkan/PipelineCacheFile.h"

#include <blake3.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace hearth::rhi {

	namespace {

		constexpr char kMagic[4] = { 'T', 'E', 'P', 'C' };
		constexpr uint32_t kVersion = 1;

		struct FileHeader
		{
			char Magic[4];
			uint32_t Version;
			uint64_t PayloadSize;
			uint32_t VendorId;
			uint32_t DeviceId;
			uint8_t CacheUuid[kPipelineCacheUuidSize];
			uint8_t PayloadHash[BLAKE3_OUT_LEN];
		};
		static_assert(sizeof(FileHeader) == 72, "the on-disk header layout is a file format");

		// VkPipelineCacheHeaderVersionOne, spelled out so this TU stays free
		// of the driver headers (the spec fixes the layout).
		struct DriverHeaderV1
		{
			uint32_t HeaderSize;
			uint32_t HeaderVersion;
			uint32_t VendorId;
			uint32_t DeviceId;
			uint8_t CacheUuid[kPipelineCacheUuidSize];
		};
		static_assert(sizeof(DriverHeaderV1) == 32, "the spec's header is 32 bytes");

		constexpr uint32_t kDriverHeaderVersionOne = 1; // VK_PIPELINE_CACHE_HEADER_VERSION_ONE

		void HashPayload(const uint8_t* payload, size_t size, uint8_t out[BLAKE3_OUT_LEN])
		{
			blake3_hasher hasher;
			blake3_hasher_init(&hasher);
			blake3_hasher_update(&hasher, payload, size);
			blake3_hasher_finalize(&hasher, out, BLAKE3_OUT_LEN);
		}
	}

	const char* PipelineCacheFileStatusName(PipelineCacheFileStatus status)
	{
		switch (status)
		{
		case PipelineCacheFileStatus::Ok: return "ok";
		case PipelineCacheFileStatus::TooSmall: return "shorter than the header";
		case PipelineCacheFileStatus::BadMagic: return "not this file format";
		case PipelineCacheFileStatus::BadVersion: return "unknown format version";
		case PipelineCacheFileStatus::IdentityMismatch: return "written by another GPU/driver/display mode";
		case PipelineCacheFileStatus::SizeMismatch: return "the payload size disagrees with the file";
		case PipelineCacheFileStatus::HashMismatch: return "the payload hash disagrees with the bytes";
		case PipelineCacheFileStatus::BadDriverHeader: return "the driver header in the payload is unusable";
		}
		return "unknown";
	}

	std::vector<uint8_t> BuildPipelineCacheFile(const uint8_t* payload, size_t payloadSize,
		const PipelineCacheIdentity& identity)
	{
		FileHeader header{};
		memcpy(header.Magic, kMagic, sizeof(kMagic));
		header.Version = kVersion;
		header.PayloadSize = payloadSize;
		header.VendorId = identity.VendorId;
		header.DeviceId = identity.DeviceId;
		memcpy(header.CacheUuid, identity.CacheUuid, sizeof(header.CacheUuid));
		HashPayload(payload, payloadSize, header.PayloadHash);

		std::vector<uint8_t> file(sizeof(FileHeader) + payloadSize);
		memcpy(file.data(), &header, sizeof(FileHeader));
		if (payloadSize > 0)
			memcpy(file.data() + sizeof(FileHeader), payload, payloadSize);
		return file;
	}

	PipelineCacheFileStatus ParsePipelineCacheFile(const std::vector<uint8_t>& file,
		const PipelineCacheIdentity& identity, const uint8_t*& payload, size_t& payloadSize)
	{
		payload = nullptr;
		payloadSize = 0;
		if (file.size() < sizeof(FileHeader))
			return PipelineCacheFileStatus::TooSmall;

		FileHeader header;
		memcpy(&header, file.data(), sizeof(FileHeader));
		if (memcmp(header.Magic, kMagic, sizeof(kMagic)) != 0)
			return PipelineCacheFileStatus::BadMagic;
		if (header.Version != kVersion)
			return PipelineCacheFileStatus::BadVersion;
		if (header.VendorId != identity.VendorId || header.DeviceId != identity.DeviceId
			|| memcmp(header.CacheUuid, identity.CacheUuid, sizeof(header.CacheUuid)) != 0)
			return PipelineCacheFileStatus::IdentityMismatch;
		if (header.PayloadSize != file.size() - sizeof(FileHeader))
			return PipelineCacheFileStatus::SizeMismatch;

		const uint8_t* bytes = file.data() + sizeof(FileHeader);
		const size_t size = file.size() - sizeof(FileHeader);
		uint8_t hash[BLAKE3_OUT_LEN];
		HashPayload(bytes, size, hash);
		if (memcmp(hash, header.PayloadHash, sizeof(hash)) != 0)
			return PipelineCacheFileStatus::HashMismatch;

		if (size < sizeof(DriverHeaderV1))
			return PipelineCacheFileStatus::BadDriverHeader;
		DriverHeaderV1 driver;
		memcpy(&driver, bytes, sizeof(DriverHeaderV1));
		if (driver.HeaderSize < sizeof(DriverHeaderV1) || driver.HeaderSize > size
			|| driver.HeaderVersion != kDriverHeaderVersionOne
			|| driver.VendorId != identity.VendorId || driver.DeviceId != identity.DeviceId
			|| memcmp(driver.CacheUuid, identity.CacheUuid, sizeof(driver.CacheUuid)) != 0)
			return PipelineCacheFileStatus::BadDriverHeader;

		payload = bytes;
		payloadSize = size;
		return PipelineCacheFileStatus::Ok;
	}

	bool WritePipelineCacheFileAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& file)
	{
		std::error_code ec;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), ec);

		// Unique per writer: two instances saving together must not share a
		// temp file either, or the interleaved write would land by rename.
		char suffix[32];
		std::random_device entropy;
		const uint64_t token = (uint64_t(entropy()) << 32) | entropy();
		snprintf(suffix, sizeof(suffix), ".%016llx.tmp", (unsigned long long)token);
		std::filesystem::path temp = path;
		temp += suffix;

		bool written = false;
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (out.is_open())
			{
				out.write((const char*)file.data(), (std::streamsize)file.size());
				out.flush();
				written = out.good();
			}
		}
		if (written)
		{
			std::filesystem::rename(temp, path, ec);
			written = !ec;
		}
		if (!written)
			std::filesystem::remove(temp, ec);
		return written;
	}
}
