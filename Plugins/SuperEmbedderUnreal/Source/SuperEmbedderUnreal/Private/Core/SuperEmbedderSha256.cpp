#include "SuperEmbedderSha256.h"

#include <cstring>

namespace SuperEmbedderUe
{
	namespace
	{
		constexpr uint32_t kRound[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

		inline uint32_t Rotr(uint32_t X, int N) { return (X >> N) | (X << (32 - N)); }
	}

	FSha256::FSha256()
		: H{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}
		, Buffer{}
	{
	}

	void FSha256::Block(const uint8_t* P)
	{
		uint32_t W[64];
		for (int I = 0; I < 16; ++I)
		{
			W[I] = static_cast<uint32_t>(P[4 * I]) << 24 | static_cast<uint32_t>(P[4 * I + 1]) << 16 |
				static_cast<uint32_t>(P[4 * I + 2]) << 8 | static_cast<uint32_t>(P[4 * I + 3]);
		}
		for (int I = 16; I < 64; ++I)
		{
			const uint32_t S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
			const uint32_t S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
			W[I] = W[I - 16] + S0 + W[I - 7] + S1;
		}
		uint32_t A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
		for (int I = 0; I < 64; ++I)
		{
			const uint32_t T1 = Hh + (Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25)) + ((E & F) ^ (~E & G)) + kRound[I] + W[I];
			const uint32_t T2 = (Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
			Hh = G;
			G = F;
			F = E;
			E = D + T1;
			D = C;
			C = B;
			B = A;
			A = T1 + T2;
		}
		H[0] += A;
		H[1] += B;
		H[2] += C;
		H[3] += D;
		H[4] += E;
		H[5] += F;
		H[6] += G;
		H[7] += Hh;
	}

	void FSha256::Update(const void* Data, size_t Size)
	{
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		TotalBytes += Size;
		while (Size > 0)
		{
			const size_t Take = (64 - BufferLen) < Size ? (64 - BufferLen) : Size;
			std::memcpy(Buffer + BufferLen, P, Take);
			BufferLen += Take;
			P += Take;
			Size -= Take;
			if (BufferLen == 64)
			{
				Block(Buffer);
				BufferLen = 0;
			}
		}
	}

	void FSha256::UpdateU32(uint32_t Value)
	{
		const uint8_t B[4] = {static_cast<uint8_t>(Value), static_cast<uint8_t>(Value >> 8),
			static_cast<uint8_t>(Value >> 16), static_cast<uint8_t>(Value >> 24)};
		Update(B, 4);
	}

	std::array<uint8_t, 32> FSha256::Final()
	{
		const uint64_t Bits = TotalBytes * 8;
		const uint8_t Pad = 0x80;
		Update(&Pad, 1);
		const uint8_t Zero = 0;
		while (BufferLen != 56)
		{
			Update(&Zero, 1);
		}
		uint8_t Len[8];
		for (int I = 0; I < 8; ++I)
		{
			Len[I] = static_cast<uint8_t>(Bits >> (8 * (7 - I)));
		}
		Update(Len, 8);
		std::array<uint8_t, 32> Out{};
		for (int I = 0; I < 8; ++I)
		{
			for (int J = 0; J < 4; ++J)
			{
				Out[static_cast<size_t>(4 * I + J)] = static_cast<uint8_t>(H[I] >> (8 * (3 - J)));
			}
		}
		return Out;
	}

	std::array<uint8_t, 32> Sha256Of(const void* Data, size_t Size)
	{
		FSha256 S;
		S.Update(Data, Size);
		return S.Final();
	}
}
