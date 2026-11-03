// The coexist pre-check binary: the SuperSLM plugin's plain engine (whatever release it vendors) and SuperEmbedderCore's
// renamed copy (v1.18.1 plus the vendored SuperEmbedder) linked into one image, as a monolithic game links
// them. Runs both engines, then the semb.h smoke on the renamed copy.
//   coexist <model.sslm> <tokenizer.sslm>
// Prints one "digest <kind> <text-id> <sha256 of the float32 vector bytes>" line per encode, which
// run.sh compares with the plain (unrenamed) build's.
#include "superembedder/semb.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" int drv_plugin(const void*, size_t, const char*, uint8_t[32]);
extern "C" int drv_embedder(const void*, size_t, const char*, uint8_t[32]);
int SmokeEncode(const char* model, const char* tokenizer);  // smoke.inc

static std::vector<unsigned char> ReadFile(const char* path) {
  std::vector<unsigned char> d;
  if (FILE* f = std::fopen(path, "rb")) {
    int c;
    while ((c = std::fgetc(f)) != EOF) d.push_back(static_cast<unsigned char>(c));
    std::fclose(f);
  }
  return d;
}

int main(int argc, char** argv) {
  if (argc != 3) { std::fprintf(stderr, "usage: coexist <model.sslm> <tokenizer.sslm>\n"); return 2; }
  const std::vector<unsigned char> m = ReadFile(argv[1]);
  uint8_t a[32], b[32];
  const int pa = drv_plugin(m.data(), m.size(), "SuperSLM plugin, plain", a);
  const int pb = drv_embedder(m.data(), m.size(), "SuperEmbedderCore, renamed v1.18.1", b);
  std::printf("engine sha256 equal across copies: %d\n", std::memcmp(a, b, 32) == 0);
  // An older plugin engine may refuse an artifact minted at the pinned engine; only the renamed copy's map is required to succeed.
  std::printf("plain plugin engine map %s (informational)\n", pa == 0 ? "ok" : "refused");
  const int smoke = SmokeEncode(argv[1], argv[2]);
  const int fail = (pb != 0) + (std::memcmp(a, b, 32) != 0) + (smoke != 0);
  std::printf("coexist: %s\n", fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
#include "smoke.inc"
