// Every core source as one translation unit, as Unreal's unity build may compile them. Built, never linked:
// it catches a name two cores both define at file scope.
#include "SuperEmbedderCoreTypes.cpp"
#include "SuperEmbedderSha256.cpp"
#include "SuperEmbedderText.cpp"
#include "SuperEmbedderEncoder.cpp"
#include "SuperEmbedderEncodeCache.cpp"
#include "SuperEmbedderEncodeService.cpp"
#include "SuperEmbedderVectorSetCore.cpp"
#include "SuperEmbedderSimilarity.cpp"
#include "SuperEmbedderRowsInput.cpp"
