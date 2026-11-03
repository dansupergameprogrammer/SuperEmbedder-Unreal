// src/version.cpp — non-arithmetic: a string constant accessor. The encoder
// itself is TE-63's.
#include "superembedder/version.h"

namespace superembedder {

const char* Version() { return SEMB_VERSION_STRING; }

}  // namespace superembedder
