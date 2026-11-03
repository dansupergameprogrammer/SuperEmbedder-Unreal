// The similarity helpers (section 2.3; cells S-1 to S-6, V-9, V-11).

#include "support.h"

#include "SuperEmbedderSimilarity.h"
#include "SuperEmbedderVectorSetCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace SuperEmbedderUe;
using namespace HostSupport;

HOST_TEST(similarity_hot_vectors_rank_as_written, "S-1")
{
	// Oracle written into the test: 1-, 2- and 3-hot vectors over 4 dims. Against q = e0:
	// e0 -> 1; e0+e1 -> 1/sqrt(2); e0+e1+e2 -> 1/sqrt(3); e1 -> 0; -e0 -> -1.
	FVectorSetCore S;
	REQUIRE_EQ(S.Append("minus", "", CheckedVector({-1, 0, 0, 0})), EStatus::Ok);
	REQUIRE_EQ(S.Append("three", "", CheckedVector({1, 1, 1, 0})), EStatus::Ok);
	REQUIRE_EQ(S.Append("other", "", CheckedVector({0, 1, 0, 0})), EStatus::Ok);
	REQUIRE_EQ(S.Append("one", "", CheckedVector({1, 0, 0, 0})), EStatus::Ok);
	REQUIRE_EQ(S.Append("two", "", CheckedVector({2, 2, 0, 0})), EStatus::Ok); // not unit length
	std::vector<FHit> H;
	REQUIRE_EQ(TopK(S.Snapshot(), CheckedVector({3, 0, 0, 0}), 5, nullptr, H), EStatus::Ok);
	REQUIRE_EQ(H.size(), size_t(5));
	const char* Order[] = {"one", "two", "three", "other", "minus"};
	const float Scores[] = {1.0f, static_cast<float>(1.0 / std::sqrt(2.0)), static_cast<float>(1.0 / std::sqrt(3.0)), 0.0f, -1.0f};
	for (size_t I = 0; I < 5; ++I)
	{
		EXPECT_EQ(H[I].Id, std::string(Order[I]));
		EXPECT_EQ(H[I].Score, Scores[I]);
	}
	float C = 0.0f;
	EXPECT_EQ(Cosine(CheckedVector({1, 1, 0, 0}), CheckedVector({0, 1, 1, 0}), C), EStatus::Ok);
	EXPECT_EQ(C, 0.5f);
}

HOST_TEST(similarity_edge_rules, "S-3, S-5")
{
	FVectorSetCore S;
	REQUIRE_EQ(S.Append("a", "", CheckedVector({1, 0})), EStatus::Ok);
	REQUIRE_EQ(S.Append("b", "", CheckedVector({0, 1})), EStatus::Ok);
	const FEmbedding Q = CheckedVector({1, 1});
	std::vector<FHit> H;
	EXPECT_EQ(TopK(S.Snapshot(), Q, 0, nullptr, H), EStatus::InvalidK);
	EXPECT_EQ(TopK(S.Snapshot(), Q, -3, nullptr, H), EStatus::InvalidK);
	EXPECT_EQ(TopK(S.Snapshot(), Q, 99, nullptr, H), EStatus::Ok);
	EXPECT_EQ(H.size(), size_t(2));
	std::unordered_set<std::string> All = {"a", "b"};
	EXPECT_EQ(TopK(S.Snapshot(), Q, 5, &All, H), EStatus::Ok);
	EXPECT_EQ(H.size(), size_t(0));
	std::unordered_set<std::string> OnlyA = {"a"};
	EXPECT_EQ(TopK(S.Snapshot(), Q, 5, &OnlyA, H), EStatus::Ok);
	REQUIRE_EQ(H.size(), size_t(1));
	EXPECT_EQ(H[0].Id, std::string("b"));
	FVectorSetCore Empty;
	EXPECT_EQ(TopK(Empty.Snapshot(), Q, 5, nullptr, H), EStatus::Ok);
	EXPECT_EQ(H.size(), size_t(0));
	EXPECT_EQ(TopK(S.Snapshot(), CheckedVector({0, 0}), 1, nullptr, H), EStatus::ZeroVector);
	EXPECT_EQ(TopK(S.Snapshot(), CheckedVector({std::numeric_limits<float>::quiet_NaN(), 1}), 1, nullptr, H), EStatus::NonFiniteVector);
	EXPECT_EQ(TopK(S.Snapshot(), CheckedVector({std::numeric_limits<float>::infinity(), 1}), 1, nullptr, H), EStatus::NonFiniteVector);
	EXPECT_EQ(TopK(S.Snapshot(), CheckedVector({1, 1, 1}), 1, nullptr, H), EStatus::DimsMismatch);
	float C = 0.0f;
	EXPECT_EQ(Cosine(CheckedVector({0, 0}), Q, C), EStatus::ZeroVector);
	EXPECT_EQ(Cosine(CheckedVector({1, 0}), CheckedVector({1, 0, 0}), C), EStatus::DimsMismatch);
}

HOST_TEST(similarity_ties_rank_the_lower_row_first, "S-4")
{
	FVectorSetCore S;
	REQUIRE_EQ(S.Append("z-first", "", CheckedVector({1, 2, 3})), EStatus::Ok);
	REQUIRE_EQ(S.Append("a-second", "", CheckedVector({1, 2, 3})), EStatus::Ok);
	REQUIRE_EQ(S.Append("m-third", "", CheckedVector({2, 4, 6})), EStatus::Ok);
	std::vector<FHit> H;
	REQUIRE_EQ(TopK(S.Snapshot(), CheckedVector({1, 2, 3}), 3, nullptr, H), EStatus::Ok);
	REQUIRE_EQ(H.size(), size_t(3));
	EXPECT_EQ(H[0].Row, size_t(0));
	EXPECT_EQ(H[1].Row, size_t(1));
	EXPECT_EQ(H[2].Row, size_t(2));
}

HOST_TEST(similarity_provenance_refusals, "V-9, V-11")
{
	FVectorSetCore SetB;
	REQUIRE_EQ(SetB.Append("b", "", CheckedVector({1, 0}, 2)), EStatus::Ok);
	std::vector<FHit> H;
	// A vector from model A against a model-B set.
	EXPECT_EQ(TopK(SetB.Snapshot(), CheckedVector({1, 0}, 1), 1, nullptr, H), EStatus::FingerprintMismatch);
	float C = 0.0f;
	EXPECT_EQ(Cosine(CheckedVector({1, 0}, 1), CheckedVector({1, 0}, 2), C), EStatus::FingerprintMismatch);
	// Unchecked vectors: refused by every helper except Cosine between two of them.
	EXPECT_EQ(TopK(SetB.Snapshot(), FEmbedding::Unchecked({1, 0}), 1, nullptr, H), EStatus::UncheckedVector);
	EXPECT_EQ(Cosine(FEmbedding::Unchecked({1, 0}), CheckedVector({1, 0}), C), EStatus::UncheckedVector);
	EXPECT_EQ(Cosine(FEmbedding::Unchecked({1, 0}), FEmbedding::Unchecked({1, 1}), C), EStatus::Ok);
	EXPECT_EQ(C, static_cast<float>(1.0 / std::sqrt(2.0)));
}

HOST_TEST(similarity_matches_an_independent_double_reference, "S-2 (on a random set)")
{
	std::mt19937 Rng(11);
	std::uniform_real_distribution<float> D(-1.0f, 1.0f);
	const uint32_t Dims = 64;
	FVectorSetCore S;
	std::vector<std::vector<float>> Rows;
	for (int I = 0; I < 200; ++I)
	{
		std::vector<float> V(Dims);
		for (float& X : V)
		{
			X = D(Rng) * 80.0f; // the published model's vectors are far from unit length
		}
		Rows.push_back(V);
		REQUIRE_EQ(S.Append("r" + std::to_string(I), "", CheckedVector(V)), EStatus::Ok);
	}
	for (int Q = 0; Q < 10; ++Q)
	{
		std::vector<float> Query(Dims);
		for (float& X : Query)
		{
			X = D(Rng);
		}
		// The reference: normalize each vector first, then dot, in long double, sorted independently.
		std::vector<std::pair<long double, int>> Ref;
		long double QN = 0;
		for (float X : Query) QN += static_cast<long double>(X) * X;
		QN = std::sqrt(QN);
		for (int I = 0; I < 200; ++I)
		{
			long double RN = 0;
			for (float X : Rows[static_cast<size_t>(I)]) RN += static_cast<long double>(X) * X;
			RN = std::sqrt(RN);
			long double Dot = 0;
			for (uint32_t J = 0; J < Dims; ++J) Dot += (Query[J] / QN) * (Rows[static_cast<size_t>(I)][J] / RN);
			Ref.emplace_back(Dot, I);
		}
		std::stable_sort(Ref.begin(), Ref.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
		std::vector<FHit> H;
		REQUIRE_EQ(TopK(S.Snapshot(), CheckedVector(Query), 10, nullptr, H), EStatus::Ok);
		REQUIRE_EQ(H.size(), size_t(10));
		for (size_t I = 0; I < 10; ++I)
		{
			EXPECT_TRUE(std::fabs(static_cast<long double>(H[I].Score) - Ref[I].first) < 1e-5L);
			const bool bSeparated = I + 1 < Ref.size() && Ref[I].first - Ref[I + 1].first > 1e-6L &&
				(I == 0 || Ref[I - 1].first - Ref[I].first > 1e-6L);
			if (bSeparated)
			{
				EXPECT_EQ(H[I].Id, "r" + std::to_string(Ref[I].second));
			}
		}
	}
}

HOST_TEST(similarity_is_reproducible, "S-6")
{
	std::mt19937 Rng(13);
	std::uniform_real_distribution<float> D(-1.0f, 1.0f);
	FVectorSetCore S;
	for (int I = 0; I < 300; ++I)
	{
		std::vector<float> V(32);
		for (float& X : V) X = D(Rng);
		REQUIRE_EQ(S.Append("r" + std::to_string(I), "", CheckedVector(V)), EStatus::Ok);
	}
	std::vector<float> Q(32);
	for (float& X : Q) X = D(Rng);
	std::vector<FHit> First;
	REQUIRE_EQ(TopK(S.Snapshot(), CheckedVector(Q), 20, nullptr, First), EStatus::Ok);
	for (int Run = 0; Run < 100; ++Run)
	{
		std::vector<FHit> H;
		REQUIRE_EQ(TopK(S.Snapshot(), CheckedVector(Q), 20, nullptr, H), EStatus::Ok);
		REQUIRE_EQ(H.size(), First.size());
		for (size_t I = 0; I < H.size(); ++I)
		{
			EXPECT_EQ(H[I].Id, First[I].Id);
			EXPECT_TRUE(std::memcmp(&H[I].Score, &First[I].Score, 4) == 0);
		}
	}
}
