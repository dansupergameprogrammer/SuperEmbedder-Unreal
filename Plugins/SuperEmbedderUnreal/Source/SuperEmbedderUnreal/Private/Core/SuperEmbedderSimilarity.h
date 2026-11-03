#pragma once

// The similarity helpers (proposal rev 9, section 2.3): exact, brute force, deterministic on one build.
// The vectors are not unit length (L2 near 82 on the published model), so every score is a cosine: the dot
// product over the product of the two norms, accumulated in double in a fixed order and rounded to float once.
//
// Edge rules (audit G-14), checked in this order:
//   TopK: k < 1 InvalidK; an unchecked query UncheckedVector; a non-finite component NonFiniteVector; a zero
//   query ZeroVector; then an empty set returns an empty list; a set of another fingerprint
//   FingerprintMismatch; other dims DimsMismatch; k above the candidate count returns every candidate; a
//   filter that excludes every row returns an empty list; equal scores rank the lower row index first.
//   Cosine: exactly one unchecked vector UncheckedVector (two unchecked vectors are allowed, V-11); two
//   fingerprints FingerprintMismatch; other dims DimsMismatch; NonFiniteVector; ZeroVector.

#include "SuperEmbedderCoreTypes.h"
#include "SuperEmbedderVectorSetCore.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace SuperEmbedderUe
{
	struct FHit
	{
		size_t Row = 0;
		std::string Id;
		float Score = 0.0f;
	};

	EStatus Cosine(const FEmbedding& A, const FEmbedding& B, float& OutScore);

	EStatus TopK(const FSetSnapshot& Set, const FEmbedding& Query, int64_t K,
		const std::unordered_set<std::string>* ExcludeIds, std::vector<FHit>& OutHits);
}
