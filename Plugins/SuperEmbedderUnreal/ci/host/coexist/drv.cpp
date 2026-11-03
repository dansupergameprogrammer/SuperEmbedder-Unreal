// Engine driver. Compiled twice: plainly against the SuperSLM plugin's vendored engine (whatever release that plugin vendors), and under
// SuperEmbedderCore's rename header against this plugin's vendored engine (v1.18.1), so each call reaches its
// own copy. Uses only the engine's public C ABI and its SHA-256.
#include "superslm/sslm_abi.h"
#include "superslm/sha256.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
extern "C" int DRV_NAME(const void* model, size_t size, const char* tag, uint8_t digest_out[32]) {
  const char* text = "drowned monastery";
  superslm::Sha256Hash(reinterpret_cast<const uint8_t*>(text), std::strlen(text), digest_out);
  sslm_model m = nullptr;
  const sslm_status st = sslm_model_map(model, size, &m);
  std::printf("[%s] sslm_model_map(u1 pair model) -> %d\n", tag, static_cast<int>(st));
  if (st == SSLM_OK) sslm_model_unmap(m);
  return st == SSLM_OK ? 0 : 1;
}
