// SuperEmbedderCore's module object. Every translation unit of this module is compiled with the engine rename
// header force-included (SuperEmbedderCore.Build.cs). This file is UE source, not engine source, so it
// includes the header a second time with SUPEREMBEDDERCORE_RENAME_UNDO defined, which removes every rename
// macro before any UE header is read. Nothing here names an engine identifier.
#define SUPEREMBEDDERCORE_RENAME_UNDO 1
#include SUPEREMBEDDERCORE_RENAME_HEADER
#undef SUPEREMBEDDERCORE_RENAME_UNDO

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, SuperEmbedderCore)
