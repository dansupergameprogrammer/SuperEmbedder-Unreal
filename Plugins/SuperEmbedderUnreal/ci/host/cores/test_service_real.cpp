// Real encodes through the vendored library (compiled as SuperEmbedderCore compiles it, under the rename
// header) on SuperEmbedder's U1 synthetic pair. The oracle is SuperEmbedder's own CMake build:
// ci/host/coexist/u1_pair_cmake_reference.txt, recorded from that build (the design's section 10.1, in
// its host form). On this synthetic pair every query composes to the same vector (the reference shows it too),
// so the query cells check equality with the reference and the document cells carry the discrimination.

#include "support.h"

#include "SuperEmbedderEncoder.h"
#include "SuperEmbedderSha256.h"
#include "SuperEmbedderSimilarity.h"
#include "SuperEmbedderVectorSetCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace SuperEmbedderUe;
using namespace HostSupport;

namespace
{
	// The smoke's three texts (ci/host/coexist/smoke.inc), in its order.
	const char* const kSmokeTexts[3] = {"the drowned monastery", "a lantern that never goes out",
		"unicode caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"};

	struct FReference
	{
		bool bLoaded = false;
		std::map<std::string, std::string> Pair; // file -> sha256
		std::string Fingerprint;
		std::string Document[3];
		std::string Query[3];
	};

	FReference LoadReference()
	{
		FReference R;
		std::ifstream In(SEMB_HOST_REFERENCE_FILE);
		std::string Line;
		while (std::getline(In, Line))
		{
			std::istringstream S(Line);
			std::string Tag;
			S >> Tag;
			if (Tag == "pair")
			{
				std::string File, Sha;
				S >> File >> Sha;
				R.Pair[File] = Sha;
			}
			else if (Tag == "fingerprint")
			{
				S >> R.Fingerprint;
			}
			else if (Tag == "digest")
			{
				std::string Kind, Hex;
				int Index = -1;
				S >> Kind >> Index >> Hex;
				if (Index >= 0 && Index < 3)
				{
					(Kind == "query" ? R.Query : R.Document)[Index] = Hex;
				}
			}
		}
		R.bLoaded = !R.Fingerprint.empty();
		return R;
	}

	bool PairIsTheReferencePair(const FReference& R)
	{
		for (const char* File : {"u1_pair_model.sslm", "u1_pair_tokenizer.sslm"})
		{
			const std::vector<uint8_t> Bytes = ReadFile(HostTest::PairDir() + "/" + File);
			auto It = R.Pair.find(File);
			if (It == R.Pair.end() || HexOf(Sha256Of(Bytes.data(), Bytes.size())) != It->second)
			{
				return false;
			}
		}
		return true;
	}

	std::unique_ptr<IEncoder> OpenDirect(const FModelSource& Source, EStatus& Status, std::string& Detail)
	{
		FSembEncoderFactory Factory(std::make_shared<FStdFiles>());
		FOpenResult R = Factory.Open(Source, 2);
		Status = R.Status;
		Detail = R.Detail;
		return static_cast<std::unique_ptr<IEncoder>&&>(R.Encoder);
	}
}

HOST_TEST(real_encodes_equal_superembedders_own_build, "O-1 and O-2 (host form), K-1, the fingerprint")
{
	const FReference Ref = LoadReference();
	REQUIRE_TRUE(Ref.bLoaded);
	REQUIRE_TRUE(!HostTest::PairDir().empty());
	REQUIRE_TRUE(PairIsTheReferencePair(Ref));
	FServiceConfig C;
	C.MaxConcurrency = 2;
	uint64_t Handle = 0;
	std::unique_ptr<FEncodeService> S = MakeRealService(C, Handle);
	REQUIRE_TRUE(S != nullptr);
	FFingerprint Fp{};
	REQUIRE_TRUE(S->GetFingerprint(Fp));
	// smoke.inc prints the SHA-256 of the 32-byte fingerprint, not the fingerprint itself.
	EXPECT_EQ(HexOf(Sha256Of(Fp.data(), Fp.size())), Ref.Fingerprint);
	EXPECT_EQ(S->GetDims(), uint32_t(32));
	// Documents through one ticket, in row order.
	std::vector<FDocumentRow> Rows;
	for (int I = 0; I < 3; ++I)
	{
		Rows.push_back({"t" + std::to_string(I), kSmokeTexts[I]});
	}
	FDelivery Docs;
	REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments(Rows, 0), Docs, 60000));
	REQUIRE_EQ(Docs.Outcome, EOutcome::Completed);
	REQUIRE_EQ(Docs.Vectors.size(), size_t(3));
	for (int I = 0; I < 3; ++I)
	{
		EXPECT_EQ(VectorDigestHex(Docs.Vectors[static_cast<size_t>(I)].Values), Ref.Document[I]);
		EXPECT_TRUE(Docs.Vectors[static_cast<size_t>(I)].Fingerprint == Fp);
	}
	// Queries, each its own ticket.
	for (int I = 0; I < 3; ++I)
	{
		FDelivery Q;
		REQUIRE_TRUE(WaitDelivery(*S, S->EncodeQuery(kSmokeTexts[I], 0), Q, 60000));
		REQUIRE_EQ(Q.Outcome, EOutcome::Completed);
		EXPECT_EQ(VectorDigestHex(Q.Vectors[0].Values), Ref.Query[I]);
		EXPECT_TRUE(Q.Vectors[0].Kind == EKind::Query);
	}
}

HOST_TEST(real_each_kind_equals_its_own_reference_in_either_order, "K-1")
{
	const FReference Ref = LoadReference();
	REQUIRE_TRUE(Ref.bLoaded && PairIsTheReferencePair(Ref));
	for (int Order = 0; Order < 2; ++Order)
	{
		FServiceConfig C;
		uint64_t Handle = 0;
		std::unique_ptr<FEncodeService> S = MakeRealService(C, Handle);
		REQUIRE_TRUE(S != nullptr);
		FDelivery Q, D;
		if (Order == 0)
		{
			REQUIRE_TRUE(WaitDelivery(*S, S->EncodeQuery(kSmokeTexts[1], 0), Q, 60000));
			REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments({{"x", kSmokeTexts[1]}}, 0), D, 60000));
		}
		else
		{
			REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments({{"x", kSmokeTexts[1]}}, 0), D, 60000));
			REQUIRE_TRUE(WaitDelivery(*S, S->EncodeQuery(kSmokeTexts[1], 0), Q, 60000));
		}
		REQUIRE_EQ(Q.Outcome, EOutcome::Completed);
		REQUIRE_EQ(D.Outcome, EOutcome::Completed);
		EXPECT_EQ(VectorDigestHex(Q.Vectors[0].Values), Ref.Query[1]);
		EXPECT_EQ(VectorDigestHex(D.Vectors[0].Values), Ref.Document[1]);
		EXPECT_NE(Ref.Query[1], Ref.Document[1]);
	}
}

HOST_TEST(real_cache_hits_equal_live_encodes, "K-6")
{
	FServiceConfig Cached;
	FServiceConfig Uncached;
	Uncached.MaxCacheEntries = 0;
	uint64_t H1 = 0, H2 = 0;
	std::unique_ptr<FEncodeService> A = MakeRealService(Cached, H1);
	std::unique_ptr<FEncodeService> B = MakeRealService(Uncached, H2);
	REQUIRE_TRUE(A != nullptr && B != nullptr);
	std::vector<FDocumentRow> Rows;
	for (int I = 0; I < 20; ++I)
	{
		Rows.push_back({"r" + std::to_string(I), "set text number " + std::to_string(I)});
	}
	FDelivery First, Second, Live;
	REQUIRE_TRUE(WaitDelivery(*A, A->EncodeDocuments(Rows, 0), First, 60000));
	const uint64_t Encodes = A->GetEncodeCount();
	REQUIRE_TRUE(WaitDelivery(*A, A->EncodeDocuments(Rows, 0), Second, 60000));
	EXPECT_EQ(A->GetEncodeCount(), Encodes); // every row a hit
	REQUIRE_TRUE(WaitDelivery(*B, B->EncodeDocuments(Rows, 0), Live, 60000));
	REQUIRE_EQ(Second.Vectors.size(), size_t(20));
	REQUIRE_EQ(Live.Vectors.size(), size_t(20));
	for (size_t I = 0; I < 20; ++I)
	{
		EXPECT_TRUE(Second.Vectors[I].Values == Live.Vectors[I].Values);
	}
	FDelivery QHit, QLive;
	REQUIRE_TRUE(WaitDelivery(*A, A->EncodeQuery("a golden phrase", 0), QHit, 60000));
	REQUIRE_TRUE(WaitDelivery(*A, A->EncodeQuery("a golden phrase", 0), QHit, 60000));
	EXPECT_TRUE(QHit.bFromCache);
	REQUIRE_TRUE(WaitDelivery(*B, B->EncodeQuery("a golden phrase", 0), QLive, 60000));
	EXPECT_TRUE(QHit.Vectors[0].Values == QLive.Vectors[0].Values);
	// The cache file round-trips the real entries.
	const std::vector<uint8_t> File = A->GetCache().Serialize();
	FEncodeCache Loaded(4096);
	std::string Detail;
	bool bCleared = false;
	EXPECT_EQ(Loaded.Load(File.data(), File.size(), Detail, bCleared), EStatus::Ok);
	EXPECT_EQ(Loaded.Num(), A->GetCache().Num());
}

HOST_TEST(real_four_workers_equal_one, "Q-8 (the documents leg; ASan in the sanitizer build)")
{
	FServiceConfig Four;
	Four.MaxConcurrency = 4;
	Four.MaxCacheEntries = 0;
	FServiceConfig One;
	One.MaxCacheEntries = 0;
	uint64_t H1 = 0, H2 = 0;
	std::unique_ptr<FEncodeService> A = MakeRealService(Four, H1);
	std::unique_ptr<FEncodeService> B = MakeRealService(One, H2);
	REQUIRE_TRUE(A != nullptr && B != nullptr);
	std::vector<FDocumentRow> Rows;
	for (int I = 0; I < 60; ++I)
	{
		Rows.push_back({"r" + std::to_string(I), "row " + std::to_string(I * 7919) + " of the corpus"});
	}
	FDelivery P, Q;
	REQUIRE_TRUE(WaitDelivery(*A, A->EncodeDocuments(Rows, 0), P, 120000));
	REQUIRE_TRUE(WaitDelivery(*B, B->EncodeDocuments(Rows, 0), Q, 120000));
	REQUIRE_EQ(P.Vectors.size(), size_t(60));
	REQUIRE_EQ(Q.Vectors.size(), size_t(60));
	for (size_t I = 0; I < 60; ++I)
	{
		EXPECT_TRUE(P.Vectors[I].Values == Q.Vectors[I].Values);
	}
	// And 16 concurrent query tickets equal the serial ones.
	std::vector<FTicket> Tickets;
	for (int I = 0; I < 16; ++I)
	{
		Tickets.push_back(A->EncodeQuery("query " + std::to_string(I), 0));
	}
	std::vector<FDelivery> All = DrainCount(*A, 16, 120000);
	REQUIRE_EQ(All.size(), size_t(16));
	for (const FDelivery& D : All)
	{
		const size_t Index = static_cast<size_t>(std::find(Tickets.begin(), Tickets.end(), D.Ticket) - Tickets.begin());
		FDelivery Serial;
		REQUIRE_TRUE(WaitDelivery(*B, B->EncodeQuery("query " + std::to_string(Index), 0), Serial, 60000));
		EXPECT_TRUE(D.Vectors[0].Values == Serial.Vectors[0].Values);
	}
}

HOST_TEST(real_memory_open_equals_path_open_and_copies_the_bytes, "L-8, L-10")
{
	EStatus St = EStatus::Ok;
	std::string Detail;
	std::unique_ptr<IEncoder> FromPath = OpenDirect(PairSource(), St, Detail);
	REQUIRE_TRUE(FromPath != nullptr);
	std::shared_ptr<std::vector<uint8_t>> Model = std::make_shared<std::vector<uint8_t>>(ReadFile(PairSource().ModelPath));
	std::shared_ptr<std::vector<uint8_t>> Tok = std::make_shared<std::vector<uint8_t>>(ReadFile(PairSource().TokenizerPath));
	FModelSource Mem;
	Mem.ModelBytes = Model;
	Mem.TokenizerBytes = Tok;
	std::unique_ptr<IEncoder> FromMemory = OpenDirect(Mem, St, Detail);
	REQUIRE_TRUE(FromMemory != nullptr);
	EXPECT_TRUE(FromPath->Fingerprint() == FromMemory->Fingerprint());
	// Overwrite the caller's spans: the open copied them (L-10).
	std::fill(Model->begin(), Model->end(), uint8_t(0xAB));
	std::fill(Tok->begin(), Tok->end(), uint8_t(0xCD));
	for (int I = 0; I < 16; ++I)
	{
		const std::string Text = "phrase number " + std::to_string(I);
		const EKind Kind = I % 2 ? EKind::Query : EKind::Document;
		std::vector<float> A, B;
		std::string Lib;
		REQUIRE_EQ(FromPath->Encode(Kind, Text, A, Lib), EStatus::Ok);
		REQUIRE_EQ(FromMemory->Encode(Kind, Text, B, Lib), EStatus::Ok);
		EXPECT_TRUE(A == B);
	}
}

HOST_TEST(real_token_bounds_at_the_edge, "Q-4, D-3 (with the library's own count)")
{
	EStatus St = EStatus::Ok;
	std::string Detail;
	std::unique_ptr<IEncoder> Enc = OpenDirect(PairSource(), St, Detail);
	REQUIRE_TRUE(Enc != nullptr);
	const std::string Text = "a lantern that never goes out";
	int64_t Count = 0;
	std::string Lib;
	REQUIRE_EQ(Enc->CountQueryBodyTokens(Text, Count, Lib), EStatus::Ok);
	REQUIRE_GT(Count, int64_t(1));
	for (int Over = 0; Over < 2; ++Over)
	{
		FServiceConfig C;
		C.MaxQueryTokens = static_cast<uint32_t>(Count) - static_cast<uint32_t>(Over);
		C.MaxDocumentTokens = C.MaxQueryTokens;
		uint64_t Handle = 0;
		std::unique_ptr<FEncodeService> S = MakeRealService(C, Handle);
		REQUIRE_TRUE(S != nullptr);
		FDelivery Q, D;
		REQUIRE_TRUE(WaitDelivery(*S, S->EncodeQuery(Text, 0), Q, 60000));
		REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments({{"ok", "short"}, {"edge", Text}}, 0), D, 60000));
		if (Over == 0)
		{
			EXPECT_EQ(Q.Outcome, EOutcome::Completed);
			EXPECT_EQ(D.Outcome, EOutcome::Completed);
		}
		else
		{
			EXPECT_EQ(Q.Status, EStatus::QueryTooLong);
			EXPECT_EQ(D.Status, EStatus::DocumentTooLong);
			EXPECT_TRUE(D.Detail.find("id 'edge'") != std::string::npos);
			EXPECT_EQ(S->GetEncodeCount(), uint64_t(0));
		}
	}
}

HOST_TEST(real_library_refusals_are_named, "Q-5 (the library's half), L-1 (a failed open names why)")
{
	EStatus St = EStatus::Ok;
	std::string Detail;
	std::unique_ptr<IEncoder> Enc = OpenDirect(PairSource(), St, Detail);
	REQUIRE_TRUE(Enc != nullptr);
	std::vector<float> V;
	std::string Lib;
	EXPECT_EQ(Enc->Encode(EKind::Document, "bad \xFF", V, Lib), EStatus::InvalidUtf8);
	EXPECT_EQ(Lib, std::string("SEMB_INVALID_UTF8"));
	EXPECT_EQ(Enc->Encode(EKind::Query, "", V, Lib), EStatus::EmptyText);
	EXPECT_EQ(Enc->Encode(EKind::Document, std::string(5000, 'x'), V, Lib), EStatus::EncodeFailed);
	EXPECT_EQ(Lib, std::string("SEMB_CONTEXT_CAP_EXCEEDED"));
	// A tokenizer without <|endoftext|>: the library refuses the open, and the name comes through.
	FModelSource Bad = PairSource();
	Bad.TokenizerPath = HostTest::PairDir() + "/u1_pair_tokenizer_no_endoftext.sslm";
	std::unique_ptr<IEncoder> None = OpenDirect(Bad, St, Detail);
	EXPECT_TRUE(None == nullptr);
	EXPECT_EQ(St, EStatus::OpenFailed);
	EXPECT_TRUE(Detail.rfind("SEMB_", 0) == 0);
	FModelSource Missing = PairSource();
	Missing.ModelPath = HostTest::PairDir() + "/no_such_model.sslm";
	OpenDirect(Missing, St, Detail);
	EXPECT_EQ(St, EStatus::ModelFileMissing);
	FModelSource WrongSha = PairSource();
	WrongSha.ExpectedTokenizerSha256 = std::string(64, 'f');
	OpenDirect(WrongSha, St, Detail);
	EXPECT_EQ(St, EStatus::ArtifactShaMismatch);
}

HOST_TEST(real_blocking_on_a_worker_equals_the_async_path, "Q-7's single-environment leg")
{
	FServiceConfig C;
	C.MaxCacheEntries = 0;
	uint64_t Handle = 0;
	std::unique_ptr<FEncodeService> S = MakeRealService(C, Handle);
	REQUIRE_TRUE(S != nullptr);
	FDelivery Async;
	REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments({{"x", kSmokeTexts[0]}}, 0), Async, 60000));
	FEmbedding Blocking;
	EStatus Status = EStatus::Count;
	std::thread T([&]() {
		std::string Detail;
		Status = S->EncodeBlocking(EKind::Document, kSmokeTexts[0], Blocking, Detail);
	});
	T.join();
	EXPECT_EQ(Status, EStatus::Ok);
	EXPECT_TRUE(Blocking.Values == Async.Vectors[0].Values);
}

HOST_TEST(real_documents_bake_into_a_set_that_ranks_itself_first, "B-1's set half, V-5 on real rows")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	uint64_t Handle = 0;
	std::unique_ptr<FEncodeService> S = MakeRealService(C, Handle);
	REQUIRE_TRUE(S != nullptr);
	std::vector<FDocumentRow> Rows;
	const char* Words[] = {"sword", "lantern", "monastery", "river", "ember", "frost", "crown", "harbor", "orchard", "tower"};
	for (int I = 0; I < 20; ++I)
	{
		Rows.push_back({"item" + std::to_string(I), std::string("the ") + Words[I % 10] + " of row " + std::to_string(I)});
	}
	FDelivery D;
	REQUIRE_TRUE(WaitDelivery(*S, S->EncodeDocuments(Rows, 0), D, 60000));
	REQUIRE_EQ(D.Outcome, EOutcome::Completed);
	FVectorSetCore Set;
	for (size_t I = 0; I < Rows.size(); ++I)
	{
		REQUIRE_EQ(Set.Append(D.Ids[I], Rows[I].Utf8, D.Vectors[I]), EStatus::Ok);
	}
	const std::vector<uint8_t> Bytes = Set.Serialize();
	FVectorSetCore Loaded;
	std::string Detail;
	REQUIRE_EQ(FVectorSetCore::Deserialize(Bytes.data(), Bytes.size(), Loaded, Detail), EStatus::Ok);
	for (size_t I = 0; I < Rows.size(); ++I)
	{
		std::vector<FHit> Hits;
		REQUIRE_EQ(TopK(Loaded.Snapshot(), D.Vectors[I], 3, nullptr, Hits), EStatus::Ok);
		REQUIRE_EQ(Hits.size(), size_t(3));
		EXPECT_EQ(Hits[0].Id, Rows[I].Id);
		EXPECT_TRUE(std::fabs(Hits[0].Score - 1.0f) < 1e-6f);
	}
}

HOST_TEST(real_non_ascii_paths_open_through_the_reader, "the MSVC narrow-path route (open from memory)")
{
	// A directory whose name is not ASCII, holding copies of the pair.
	const std::string Dir = "/tmp/semb_cores_\xC3\xA9t\xC3\xA9_" + std::to_string(::getpid());
	REQUIRE_TRUE(::mkdir(Dir.c_str(), 0700) == 0);
	FModelSource Src;
	Src.ModelPath = Dir + "/mod\xC3\xA8le.sslm";
	Src.TokenizerPath = Dir + "/tokenizer.sslm";
	for (int I = 0; I < 2; ++I)
	{
		const std::vector<uint8_t> Bytes = ReadFile(I == 0 ? PairSource().ModelPath : PairSource().TokenizerPath);
		std::FILE* F = std::fopen((I == 0 ? Src.ModelPath : Src.TokenizerPath).c_str(), "wb");
		REQUIRE_TRUE(F != nullptr);
		std::fwrite(Bytes.data(), 1, Bytes.size(), F);
		std::fclose(F);
	}
	EStatus St = EStatus::Ok;
	std::string Detail;
	std::unique_ptr<IEncoder> ByPath = OpenDirect(PairSource(), St, Detail);
	FSembEncoderFactory Reading(std::make_shared<FStdFiles>(), true);
	FOpenResult R = Reading.Open(Src, 1);
	EXPECT_EQ(R.Status, EStatus::Ok);
	REQUIRE_TRUE(R.Encoder != nullptr && ByPath != nullptr);
	EXPECT_TRUE(R.Encoder->Fingerprint() == ByPath->Fingerprint());
	std::remove(Src.ModelPath.c_str());
	std::remove(Src.TokenizerPath.c_str());
	::rmdir(Dir.c_str());
}
