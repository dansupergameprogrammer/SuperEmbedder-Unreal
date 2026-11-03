#pragma once

// A self-contained SHA-256 for the cores: the cache key, the set row digest, the cache entry digest and the
// vector digest. The plugin's own, so the runtime module never names a symbol of either engine copy.

#include <array>
#include <cstddef>
#include <cstdint>

namespace SuperEmbedderUe
{
	class FSha256
	{
	public:
		FSha256();
		void Update(const void* Data, size_t Size);
		void UpdateU32(uint32_t Value);
		std::array<uint8_t, 32> Final();

	private:
		void Block(const uint8_t* P);
		uint32_t H[8];
		uint8_t Buffer[64];
		size_t BufferLen = 0;
		uint64_t TotalBytes = 0;
	};

	std::array<uint8_t, 32> Sha256Of(const void* Data, size_t Size);
}
