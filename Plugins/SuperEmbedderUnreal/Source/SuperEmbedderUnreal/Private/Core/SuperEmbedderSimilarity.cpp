#include "SuperEmbedderSimilarity.h"

#include <algorithm>
#include <utility>

namespace SuperEmbedderUe
{
	namespace
	{
		double Dot(const float* A, const float* B, size_t N)
		{
			double Sum = 0.0;
			for (size_t I = 0; I < N; ++I)
			{
				Sum += static_cast<double>(A[I]) * static_cast<double>(B[I]);
			}
			return Sum;
		}
	}

	EStatus Cosine(const FEmbedding& A, const FEmbedding& B, float& OutScore)
	{
		if (A.bChecked != B.bChecked)
		{
			return EStatus::UncheckedVector;
		}
		if (A.bChecked && A.Fingerprint != B.Fingerprint)
		{
			return EStatus::FingerprintMismatch;
		}
		if (A.Values.size() != B.Values.size())
		{
			return EStatus::DimsMismatch;
		}
		double NormA = 0.0;
		double NormB = 0.0;
		const EStatus SA = CheckRowValues(A.Values, NormA);
		if (SA != EStatus::Ok)
		{
			return SA;
		}
		const EStatus SB = CheckRowValues(B.Values, NormB);
		if (SB != EStatus::Ok)
		{
			return SB;
		}
		OutScore = static_cast<float>(Dot(A.Values.data(), B.Values.data(), A.Values.size()) / (NormA * NormB));
		return EStatus::Ok;
	}

	EStatus TopK(const FSetSnapshot& Set, const FEmbedding& Query, int64_t K,
		const std::unordered_set<std::string>* ExcludeIds, std::vector<FHit>& OutHits)
	{
		OutHits.clear();
		if (K < 1)
		{
			return EStatus::InvalidK;
		}
		if (!Query.bChecked)
		{
			return EStatus::UncheckedVector;
		}
		double QueryNorm = 0.0;
		const EStatus QS = CheckRowValues(Query.Values, QueryNorm);
		if (QS != EStatus::Ok)
		{
			return QS;
		}
		if (Set.Num() == 0)
		{
			return EStatus::Ok;
		}
		if (!Set.bHasFingerprint || Set.Fingerprint != Query.Fingerprint)
		{
			return EStatus::FingerprintMismatch;
		}
		if (Query.Values.size() != Set.Dims)
		{
			return EStatus::DimsMismatch;
		}
		const std::vector<FSetRowPtr>& Rows = *Set.Rows;
		std::vector<std::pair<double, size_t>> Scored;
		Scored.reserve(Rows.size());
		for (size_t I = 0; I < Rows.size(); ++I)
		{
			const FSetRow& R = *Rows[I];
			if (ExcludeIds != nullptr && ExcludeIds->find(R.Id) != ExcludeIds->end())
			{
				continue;
			}
			Scored.emplace_back(Dot(Query.Values.data(), R.Values.data(), Set.Dims) / (QueryNorm * R.Norm), I);
		}
		const size_t Take = static_cast<uint64_t>(K) < Scored.size() ? static_cast<size_t>(K) : Scored.size();
		auto Better = [](const std::pair<double, size_t>& A, const std::pair<double, size_t>& B) {
			return A.first != B.first ? A.first > B.first : A.second < B.second;
		};
		std::partial_sort(Scored.begin(), Scored.begin() + static_cast<std::ptrdiff_t>(Take), Scored.end(), Better);
		OutHits.reserve(Take);
		for (size_t I = 0; I < Take; ++I)
		{
			FHit H;
			H.Row = Scored[I].second;
			H.Id = Rows[H.Row]->Id;
			H.Score = static_cast<float>(Scored[I].first);
			OutHits.push_back(static_cast<FHit&&>(H));
		}
		return EStatus::Ok;
	}
}
