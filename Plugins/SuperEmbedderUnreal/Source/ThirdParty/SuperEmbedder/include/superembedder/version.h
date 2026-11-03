// include/superembedder/version.h — the version of the `superembedder`
// library and the `semb` CLI. It matches the CMake project version.
#pragma once

#define SEMB_VERSION_MAJOR 1
#define SEMB_VERSION_MINOR 0
#define SEMB_VERSION_PATCH 0
#define SEMB_VERSION_STRING "1.0.0"

namespace superembedder {

// Returns SEMB_VERSION_STRING.
const char* Version();

}  // namespace superembedder
