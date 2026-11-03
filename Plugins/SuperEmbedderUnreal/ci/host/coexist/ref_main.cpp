// The plain reference: SuperEmbedder and its pinned engine compiled WITHOUT the rename header, alone in
// the image. Its digests are what the renamed copy must reproduce (the rename changes link names only).
#include "superembedder/semb.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
int SmokeEncode(const char* model, const char* tokenizer);
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  return SmokeEncode(argv[1], argv[2]);
}
#include "smoke.inc"
