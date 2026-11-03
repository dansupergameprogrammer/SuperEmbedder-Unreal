// The exact encode cache (section 2.2; cells K-3, K-4, K-5's core half, K-7, K-8).

#include "support.h"

#include "SuperEmbedderEncodeCache.h"

#include <thread>

using namespace SuperEmbedderUe;
using namespace HostSupport;

namespace
{
	std::vector<float> Row(float Seed) { return {Seed, Seed + 1.0f, -Seed, 0.5f}; }
	FCacheKey Key(int I, EKind Kind = EKind::Query) { return FCacheKey::Of(Kind, "text " + std::to_string(I)); }
	void Put(FEncodeCache& C, const FCacheKey& K, const std::vector<float>& V)
	{
		std::vector<float> Ignored;
		if (C.Acquire(K, Ignored) == FEncodeCache::EAcquire::Owner)
		{
			C.Fill(K, V);
		}
	}
}

HOST_TEST(cache_kinds_do_not_meet, "K-1's key half")
{
	FEncodeCache C(16);
	C.BindFingerprint(FakeFingerprint(1));
	Put(C, FCacheKey::Of(EKind::Query, "T"), Row(1));
	std::vector<float> Out;
	EXPECT_TRUE(!C.Find(FCacheKey::Of(EKind::Document, "T"), Out));
	EXPECT_TRUE(C.Find(FCacheKey::Of(EKind::Query, "T"), Out));
	EXPECT_TRUE(Out == Row(1));
}

HOST_TEST(cache_lru_keeps_the_used_oldest, "K-3")
{
	const size_t Max = 4096;
	FEncodeCache C(Max);
	C.BindFingerprint(FakeFingerprint(1));
	for (size_t I = 0; I < Max; ++I)
	{
		Put(C, Key(static_cast<int>(I)), Row(static_cast<float>(I)));
	}
	std::vector<float> Out;
	REQUIRE_TRUE(C.Find(Key(0), Out)); // the oldest is read, so it is now the newest
	Put(C, Key(static_cast<int>(Max)), Row(-1.0f));
	EXPECT_TRUE(C.Find(Key(0), Out));   // survives
	EXPECT_TRUE(!C.Find(Key(1), Out));  // the second-oldest was evicted
	EXPECT_EQ(C.Num(), Max);
	// A first-in-first-out cache would have evicted entry 0: the cell's must-reject is the line above.
	for (int I = 0; I < 10000; ++I)
	{
		Put(C, Key(100000 + I), Row(1.0f));
		REQUIRE_TRUE(C.Num() <= Max);
	}
	EXPECT_EQ(C.Num(), Max);
}

HOST_TEST(cache_coalesces_two_misses_on_one_key, "K-5 (core)")
{
	FEncodeCache C(16);
	C.BindFingerprint(FakeFingerprint(1));
	const FCacheKey K = Key(7);
	std::vector<float> First;
	REQUIRE_TRUE(C.Acquire(K, First) == FEncodeCache::EAcquire::Owner);
	std::vector<float> Second;
	FEncodeCache::EAcquire SecondResult = FEncodeCache::EAcquire::Owner;
	std::thread Waiter([&]() { SecondResult = C.Acquire(K, Second); });
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	C.Fill(K, Row(7));
	Waiter.join();
	EXPECT_TRUE(SecondResult == FEncodeCache::EAcquire::Hit);
	EXPECT_TRUE(Second == Row(7));
	// An abandoned owner hands the key to the next waiter.
	const FCacheKey K2 = Key(8);
	REQUIRE_TRUE(C.Acquire(K2, First) == FEncodeCache::EAcquire::Owner);
	std::thread Waiter2([&]() { SecondResult = C.Acquire(K2, Second); });
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	C.Abandon(K2);
	Waiter2.join();
	EXPECT_TRUE(SecondResult == FEncodeCache::EAcquire::Owner);
	C.Abandon(K2);
}

HOST_TEST(cache_file_round_trips_in_use_order, "K-4 accept, K-6's persistence half")
{
	FEncodeCache C(8);
	C.BindFingerprint(FakeFingerprint(1));
	for (int I = 0; I < 5; ++I)
	{
		Put(C, Key(I, I % 2 ? EKind::Document : EKind::Query), Row(static_cast<float>(I)));
	}
	std::vector<float> Out;
	C.Find(Key(0), Out);
	const std::vector<uint8_t> Bytes = C.Serialize();
	FEncodeCache D(8);
	std::string Detail;
	bool bCleared = true;
	REQUIRE_EQ(D.Load(Bytes.data(), Bytes.size(), Detail, bCleared), EStatus::Ok);
	EXPECT_TRUE(!bCleared);
	EXPECT_EQ(D.Num(), size_t(5));
	EXPECT_TRUE(D.Serialize() == Bytes);
	EXPECT_TRUE(D.Find(Key(3, EKind::Document), Out));
	EXPECT_TRUE(Out == Row(3));
	// Binding the same fingerprint keeps it; another clears it (K-7).
	EXPECT_TRUE(!D.BindFingerprint(FakeFingerprint(1)));
	EXPECT_EQ(D.Num(), size_t(5));
	EXPECT_TRUE(D.BindFingerprint(FakeFingerprint(2)));
	EXPECT_EQ(D.Num(), size_t(0));
}

HOST_TEST(cache_file_refusals_are_named_and_leave_the_cache_cold, "K-4, K-8")
{
	FEncodeCache C(8);
	C.BindFingerprint(FakeFingerprint(1));
	Put(C, Key(1), Row(1));
	Put(C, Key(2), Row(2));
	const std::vector<uint8_t> Good = C.Serialize();
	auto LoadInto = [](const std::vector<uint8_t>& B, size_t& OutNum) {
		FEncodeCache Target(8);
		Target.BindFingerprint(FakeFingerprint(1));
		std::string Detail;
		bool bCleared = false;
		const EStatus S = Target.Load(B.data(), B.size(), Detail, bCleared);
		OutNum = Target.Num();
		return S;
	};
	size_t Num = 0;
	std::vector<uint8_t> B = Good;
	B[0] = 'X';
	EXPECT_EQ(LoadInto(B, Num), EStatus::BadMagic);
	EXPECT_EQ(Num, size_t(0));
	B = Good;
	B[4] = 2;
	EXPECT_EQ(LoadInto(B, Num), EStatus::UnsupportedVersion);
	for (size_t Len = 0; Len < Good.size(); ++Len)
	{
		B.assign(Good.begin(), Good.begin() + static_cast<std::ptrdiff_t>(Len));
		EXPECT_EQ(LoadInto(B, Num), EStatus::Truncated);
		EXPECT_EQ(Num, size_t(0));
	}
	// One flipped byte inside an entry's floats, and inside its stored digest (K-8).
	B = Good;
	B[kCacheFileHeaderBytes + 1 + 32 + 2] ^= 0x01;
	EXPECT_EQ(LoadInto(B, Num), EStatus::EntryDigestMismatch);
	EXPECT_EQ(Num, size_t(0));
	B = Good;
	B[B.size() - 1] ^= 0x80;
	EXPECT_EQ(LoadInto(B, Num), EStatus::EntryDigestMismatch);
	B = Good;
	B.push_back(0);
	EXPECT_EQ(LoadInto(B, Num), EStatus::Malformed);
	B = Good;
	B[kCacheFileHeaderBytes] = 5; // kind byte
	EXPECT_EQ(LoadInto(B, Num), EStatus::Malformed);
	// The good file loads (the control: the refusals above are the cells', not the reader's).
	EXPECT_EQ(LoadInto(Good, Num), EStatus::Ok);
	EXPECT_EQ(Num, size_t(2));
}

HOST_TEST(cache_file_of_another_fingerprint_is_cleared, "K-7")
{
	FEncodeCache C(8);
	C.BindFingerprint(FakeFingerprint(2));
	Put(C, Key(1), Row(1));
	const std::vector<uint8_t> Other = C.Serialize();
	FEncodeCache D(8);
	D.BindFingerprint(FakeFingerprint(1));
	Put(D, Key(9), Row(9));
	std::string Detail;
	bool bCleared = false;
	EXPECT_EQ(D.Load(Other.data(), Other.size(), Detail, bCleared), EStatus::Ok);
	EXPECT_TRUE(bCleared);
	EXPECT_EQ(D.Num(), size_t(0));
	// Unbound, the file binds its own fingerprint, and the open's bind clears it when the encoder differs.
	FEncodeCache E(8);
	EXPECT_EQ(E.Load(Other.data(), Other.size(), Detail, bCleared), EStatus::Ok);
	EXPECT_EQ(E.Num(), size_t(1));
	EXPECT_TRUE(E.BindFingerprint(FakeFingerprint(1)));
	EXPECT_EQ(E.Num(), size_t(0));
}
