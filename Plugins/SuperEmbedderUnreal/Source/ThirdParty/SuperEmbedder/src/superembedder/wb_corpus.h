// src/superembedder/wb_corpus.h -- TE-147 (U1 slice 2 build): declares the
// sixteen compiled-in W-B witness documents (wb_corpus.cpp's own header
// comment states why they are compiled in rather than read from a file at
// runtime). Internal to the library -- not part of the frozen public ABI
// and not installed.
#ifndef SUPEREMBEDDER_WB_CORPUS_H
#define SUPEREMBEDDER_WB_CORPUS_H

#include <cstddef>

namespace semb {

struct WbCorpusDocument {
  const char* text;
  size_t text_len;
};

inline constexpr size_t kWbCorpusDocumentCount = 16;

// Defined in wb_corpus.cpp; index order matches the committed fixture
// tests/fixtures/wb/wb-corpus.jsonl's own ids wb-00..wb-15.
extern const WbCorpusDocument kWbCorpusDocuments[kWbCorpusDocumentCount];

}  // namespace semb

#endif  // SUPEREMBEDDER_WB_CORPUS_H
