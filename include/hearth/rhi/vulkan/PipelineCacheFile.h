#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

// The on-disk container around the driver's pipeline cache blob. Pure
// bytes + file IO (no driver calls) so the rules run in tinytest: a blob
// is handed to the driver only when it is whole (NVIDIA answers a damaged
// body with VK_ERROR_UNKNOWN instead of ignoring it, and silently accepts
// flipped body bytes), and a save can never be observed half-written by
// another app instance.
namespace hearth::rhi {

	inline constexpr size_t kPipelineCacheUuidSize = 16; // VK_UUID_SIZE

	struct PipelineCacheIdentity
	{
		uint32_t VendorId = 0;
		uint32_t DeviceId = 0;
		uint8_t CacheUuid[kPipelineCacheUuidSize] = {};
	};

	enum class PipelineCacheFileStatus : uint8_t
	{
		Ok = 0,
		TooSmall,         // shorter than the container header
		BadMagic,         // not this container (includes the old UUID-prefixed format)
		BadVersion,       // a container revision this build cannot read
		IdentityMismatch, // another GPU/driver/display mode wrote it - expected, not damage
		SizeMismatch,     // the recorded payload size disagrees with the file
		HashMismatch,     // the payload's bytes are damaged
		BadDriverHeader,  // the payload's VkPipelineCacheHeaderVersionOne is unusable
	};

	const char* PipelineCacheFileStatusName(PipelineCacheFileStatus status);

	// Wraps a driver blob into the file image.
	std::vector<uint8_t> BuildPipelineCacheFile(const uint8_t* payload, size_t payloadSize,
		const PipelineCacheIdentity& identity);

	// Validates a file image. On Ok, payload/payloadSize point into `file`.
	PipelineCacheFileStatus ParsePipelineCacheFile(const std::vector<uint8_t>& file,
		const PipelineCacheIdentity& identity, const uint8_t*& payload, size_t& payloadSize);

	// Writes a per-writer unique sibling temp file, then renames it over
	// `path`: a concurrent reader or writer sees the whole old file or the
	// whole new one (last writer wins), never a mix. False = `path` is
	// untouched and the temp file is gone.
	bool WritePipelineCacheFileAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& file);
}
