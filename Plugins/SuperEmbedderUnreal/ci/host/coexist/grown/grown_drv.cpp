// Grown-struct fixture driver. Compiled twice into one image: plainly against the SuperSLM plugin's engine
// (DRV_NAME=drv_plugin), and under the grown tree's rename header against the grown engine
// (DRV_NAME=drv_embedder), so each call reaches its own copy. Uses only the engine's public C ABI.
//
// Per copy: map the two-schema model (fills the model's std::vector<sslm_schema_s> of schema handles), make a
// K/V pool (constructs an sslm_kv_pool_s) and a sequence from it, look up both schemas and bind each to the
// sequence, then release everything. Returns the number of failed steps.
#include "superslm/sslm_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>
#if defined(_MSC_VER)
#include <malloc.h>
static void* AlignedAlloc(size_t a, size_t n) { return _aligned_malloc(n, a); }
static void AlignedFree(void* p) { _aligned_free(p); }
#else
static void* AlignedAlloc(size_t a, size_t n) { return std::aligned_alloc(a, n); }
static void AlignedFree(void* p) { std::free(p); }
#endif

extern "C" int DRV_NAME(const void* data, size_t size, const char* tag) {
	int bad = 0;
	sslm_model m = nullptr;
	sslm_status st = sslm_model_map(data, size, &m);
	std::printf("[%s] sslm_model_map -> %d\n", tag, (int)st);
	if (st != SSLM_OK) return 1;
	const size_t a = SSLM_ABI_ALIGNMENT_BYTES;
	const size_t need = sslm_kv_pool_overhead_size(m, 1) + sslm_kv_block_size(m);
	const size_t bytes = (need + a - 1) / a * a;
	void* buf = AlignedAlloc(a, bytes);
	sslm_kv_pool pool = nullptr;
	st = sslm_kv_pool_create(m, buf, bytes, 1, &pool);
	std::printf("[%s] sslm_kv_pool_create -> %d\n", tag, (int)st);
	if (st != SSLM_OK) ++bad;
	sslm_seq seq = nullptr;
	if (pool) {
		st = sslm_seq_create(m, &pool, &seq);
		std::printf("[%s] sslm_seq_create -> %d\n", tag, (int)st);
		if (st != SSLM_OK) ++bad;
	}
	for (const char* name : {"grown_accepts_q", "grown_rejects_q"}) {
		sslm_schema s = nullptr;
		const sslm_status l = sslm_schema_lookup(m, name, &s);
		const sslm_status b = seq ? sslm_seq_set_schema(seq, s) : SSLM_INVALID_ARGUMENT;
		std::printf("[%s]   schema %s: lookup -> %d, bind -> %d\n", tag, name, (int)l, (int)b);
		if (l != SSLM_OK || b != SSLM_OK) ++bad;
		if (seq) sslm_seq_set_schema(seq, nullptr);
	}
	if (seq) sslm_seq_release(seq);
	if (pool && sslm_kv_pool_destroy(pool) != SSLM_OK) ++bad;
	if (sslm_model_unmap(m) != SSLM_OK) ++bad;
	AlignedFree(buf);
	std::printf("[%s] failures: %d\n", tag, bad);
	return bad;
}
