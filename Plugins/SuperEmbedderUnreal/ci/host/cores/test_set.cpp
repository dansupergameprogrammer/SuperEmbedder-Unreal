// The vector set (section 2.3; cells V-1, V-2, V-5, V-6, V-7, V-8, V-9's set half, V-11's set half, V-12's
// snapshot half).

#include "support.h"

#include "SuperEmbedderSimilarity.h"
#include "SuperEmbedderVectorSetCore.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <thread>

using namespace SuperEmbedderUe;
using namespace HostSupport;

namespace
{
	std::vector<float> RandomRow(std::mt19937& Rng, uint32_t Dims)
	{
		std::uniform_real_distribution<float> D(-1.0f, 1.0f);
		std::vector<float> V(Dims);
		for (float& X : V)
		{
			X = D(Rng);
		}
		return V;
	}

	bool SameHits(const std::vector<FHit>& A, const std::vector<FHit>& B)
	{
		if (A.size() != B.size())
		{
			return false;
		}
		for (size_t I = 0; I < A.size(); ++I)
		{
			uint32_t BitsA = 0;
			uint32_t BitsB = 0;
			std::memcpy(&BitsA, &A[I].Score, 4);
			std::memcpy(&BitsB, &B[I].Score, 4);
			if (A[I].Id != B[I].Id || BitsA != BitsB)
			{
				return false;
			}
		}
		return true;
	}
}

HOST_TEST(set_append_refusals, "V-1, V-7, V-11")
{
	FVectorSetCore S;
	EXPECT_EQ(S.Append("a", "x", FEmbedding::Unchecked({1, 0, 0})), EStatus::UncheckedVector);
	REQUIRE_EQ(S.Append("a", "x", CheckedVector({1, 0, 0})), EStatus::Ok);
	EXPECT_TRUE(S.HasFingerprint());
	EXPECT_EQ(S.GetDims(), uint32_t(3));
	EXPECT_EQ(S.Append("a", "y", CheckedVector({0, 1, 0})), EStatus::DuplicateId);
	EXPECT_EQ(S.Append("b", "y", CheckedVector({0, 1, 0}, 2)), EStatus::FingerprintMismatch);
	EXPECT_EQ(S.Append("b", "y", CheckedVector({0, 1, 0, 0})), EStatus::DimsMismatch);
	EXPECT_EQ(S.Append("b", "y", CheckedVector({0, 0, 0})), EStatus::ZeroVector);
	EXPECT_EQ(S.Append("b", "y", CheckedVector({0, std::numeric_limits<float>::quiet_NaN(), 0})), EStatus::NonFiniteVector);
	EXPECT_EQ(S.Append("b", "y", CheckedVector({0, std::numeric_limits<float>::infinity(), 0})), EStatus::NonFiniteVector);
	EXPECT_EQ(S.Append("", "y", CheckedVector({0, 1, 0})), EStatus::InvalidId);
	EXPECT_EQ(S.Num(), size_t(1));
	EXPECT_EQ(S.Remove("zzz"), EStatus::UnknownId);
	EXPECT_EQ(S.Remove("a"), EStatus::Ok);
	EXPECT_EQ(S.Num(), size_t(0));
	EXPECT_EQ(S.Remove("a"), EStatus::UnknownId);
	// The emptied set still belongs to its encoder.
	EXPECT_EQ(S.Append("c", "z", CheckedVector({0, 1, 0}, 2)), EStatus::FingerprintMismatch);
	EXPECT_EQ(S.Append("c", "z", CheckedVector({0, 1, 0})), EStatus::Ok);
}

HOST_TEST(set_lifetime_oracle, "V-2")
{
	std::mt19937 Rng(42);
	const uint32_t Dims = 16;
	std::vector<std::vector<float>> Values;
	for (int I = 0; I < 55; ++I)
	{
		Values.push_back(RandomRow(Rng, Dims));
	}
	FVectorSetCore Live;
	for (int I = 0; I < 50; ++I)
	{
		REQUIRE_EQ(Live.Append("r" + std::to_string(I), "t", CheckedVector(Values[static_cast<size_t>(I)])), EStatus::Ok);
	}
	for (int I = 0; I < 50; I += 5)
	{
		REQUIRE_EQ(Live.Remove("r" + std::to_string(I)), EStatus::Ok);
	}
	for (int I = 50; I < 55; ++I)
	{
		REQUIRE_EQ(Live.Append("r" + std::to_string(I), "t", CheckedVector(Values[static_cast<size_t>(I)])), EStatus::Ok);
	}
	FVectorSetCore Fresh;
	for (size_t I = 0; I < Live.Num(); ++I)
	{
		const std::string& Id = Live.Row(I).Id;
		const int N = std::stoi(Id.substr(1));
		REQUIRE_EQ(Fresh.Append(Id, "t", CheckedVector(Values[static_cast<size_t>(N)])), EStatus::Ok);
	}
	const FEmbedding Q = CheckedVector(RandomRow(Rng, Dims));
	std::vector<FHit> A;
	std::vector<FHit> B;
	REQUIRE_EQ(TopK(Live.Snapshot(), Q, 45, nullptr, A), EStatus::Ok);
	REQUIRE_EQ(TopK(Fresh.Snapshot(), Q, 45, nullptr, B), EStatus::Ok);
	EXPECT_EQ(A.size(), size_t(45));
	EXPECT_TRUE(SameHits(A, B));
	EXPECT_TRUE(Live.Serialize() == Fresh.Serialize());
}

HOST_TEST(set_save_load_round_trips, "V-5")
{
	std::mt19937 Rng(7);
	FVectorSetCore S;
	for (int I = 0; I < 20; ++I)
	{
		REQUIRE_EQ(S.Append("id" + std::to_string(I), "text \xC3\xA9 " + std::to_string(I), CheckedVector(RandomRow(Rng, 8))), EStatus::Ok);
	}
	const std::vector<uint8_t> Bytes = S.Serialize();
	FVectorSetCore L;
	std::string Detail;
	REQUIRE_EQ(FVectorSetCore::Deserialize(Bytes.data(), Bytes.size(), L, Detail), EStatus::Ok);
	EXPECT_TRUE(L.Serialize() == Bytes);
	EXPECT_TRUE(L.ComputeRowDigest() == S.ComputeRowDigest());
	EXPECT_EQ(L.Row(3).Text, std::string("text \xC3\xA9 3"));
	EXPECT_EQ(L.IndexOf("id7"), int64_t(7));
	// An empty set without a fingerprint round-trips too.
	FVectorSetCore E;
	const std::vector<uint8_t> EB = E.Serialize();
	EXPECT_EQ(FVectorSetCore::Deserialize(EB.data(), EB.size(), L, Detail), EStatus::Ok);
	EXPECT_EQ(L.Num(), size_t(0));
	EXPECT_TRUE(!L.HasFingerprint());
}

HOST_TEST(set_read_refusals_are_named, "V-6, V-8")
{
	FVectorSetCore S;
	REQUIRE_EQ(S.Append("a", "alpha", CheckedVector({1, 2, 3, 4})), EStatus::Ok);
	REQUIRE_EQ(S.Append("b", "beta", CheckedVector({4, 3, 2, 1})), EStatus::Ok);
	const std::vector<uint8_t> Good = S.Serialize();
	FVectorSetCore Out;
	REQUIRE_EQ(Out.Append("keep", "k", CheckedVector({1, 1, 1, 1})), EStatus::Ok);
	std::string Detail;
	std::vector<uint8_t> B = Good;
	B[1] = 'X';
	EXPECT_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Out, Detail), EStatus::BadMagic);
	B = Good;
	B[4] = 9;
	EXPECT_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Out, Detail), EStatus::UnsupportedVersion);
	for (size_t Len = 0; Len < Good.size(); ++Len)
	{
		EXPECT_EQ(FVectorSetCore::Deserialize(Good.data(), Len, Out, Detail), EStatus::Truncated);
	}
	B = Good;
	B.push_back(0);
	EXPECT_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Out, Detail), EStatus::Malformed);
	B = Good;
	B[8] = 3; // an unknown flag bit
	EXPECT_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Out, Detail), EStatus::Malformed);
	// One flipped byte in each region a row holds: id, text, floats. The digest catches each (V-8).
	const size_t RowStart = kSetFileHeaderBytes;
	const size_t Offsets[] = {RowStart + 4, RowStart + 4 + 1 + 4 + 2, RowStart + 4 + 1 + 4 + 5 + 3, Good.size() - 1};
	for (size_t Off : Offsets)
	{
		B = Good;
		B[Off] ^= 0x01;
		EXPECT_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Out, Detail), EStatus::SetCorrupt);
	}
	// A flipped fingerprint byte reads, and then refuses FingerprintMismatch against the encoder's vectors (V-1).
	B = Good;
	B[12] ^= 0x01;
	FVectorSetCore Flipped;
	REQUIRE_EQ(FVectorSetCore::Deserialize(B.data(), B.size(), Flipped, Detail), EStatus::Ok);
	std::vector<FHit> Hits;
	EXPECT_EQ(TopK(Flipped.Snapshot(), CheckedVector({1, 0, 0, 0}), 1, nullptr, Hits), EStatus::FingerprintMismatch);
	// Out was left as it was by every refusal.
	EXPECT_EQ(Out.Num(), size_t(1));
	EXPECT_TRUE(Out.Contains("keep"));
}

HOST_TEST(set_snapshot_is_unchanged_by_later_mutation, "V-12 (snapshot), V-3's core half")
{
	std::mt19937 Rng(3);
	FVectorSetCore S;
	for (int I = 0; I < 200; ++I)
	{
		REQUIRE_EQ(S.Append("r" + std::to_string(I), "", CheckedVector(RandomRow(Rng, 32))), EStatus::Ok);
	}
	const FEmbedding Q = CheckedVector(RandomRow(Rng, 32));
	std::vector<FHit> Sync;
	REQUIRE_EQ(TopK(S.Snapshot(), Q, 10, nullptr, Sync), EStatus::Ok);
	const FSetSnapshot Snap = S.Snapshot();
	std::vector<FHit> Async;
	EStatus AsyncStatus = EStatus::Ok;
	std::thread Worker([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		AsyncStatus = TopK(Snap, Q, 10, nullptr, Async);
	});
	// The game thread removes the best row and appends others while the ranking runs.
	EXPECT_EQ(S.Remove(Sync[0].Id), EStatus::Ok);
	for (int I = 0; I < 50; ++I)
	{
		EXPECT_EQ(S.Append("n" + std::to_string(I), "", CheckedVector(RandomRow(Rng, 32))), EStatus::Ok);
	}
	Worker.join();
	EXPECT_EQ(AsyncStatus, EStatus::Ok);
	EXPECT_TRUE(SameHits(Async, Sync));
	// A ranking started after the Remove sees it, by construction.
	std::vector<FHit> After;
	REQUIRE_EQ(TopK(S.Snapshot(), Q, 10, nullptr, After), EStatus::Ok);
	EXPECT_NE(After[0].Id, Sync[0].Id);
}

HOST_TEST(set_large_snapshot_is_cheap, "V-12 (game-thread cost of starting over 50,000 rows)")
{
	std::mt19937 Rng(5);
	FVectorSetCore S;
	for (int I = 0; I < 50000; ++I)
	{
		REQUIRE_EQ(S.Append("r" + std::to_string(I), "", CheckedVector(RandomRow(Rng, 8))), EStatus::Ok);
	}
	const auto T0 = std::chrono::steady_clock::now();
	const FSetSnapshot Snap = S.Snapshot();
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	EXPECT_EQ(Snap.Num(), size_t(50000));
	EXPECT_LT(Ms, 1.0);
}
