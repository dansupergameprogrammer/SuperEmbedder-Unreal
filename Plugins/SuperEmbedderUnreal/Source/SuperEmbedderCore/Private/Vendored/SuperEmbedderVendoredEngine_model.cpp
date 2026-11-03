// Vendored-source wrapper: one translation unit per engine source, as in the engine's own build (several
// engine sources define same-named helpers in anonymous namespaces, so two in one TU collide). This module
// compiles the engine under the force-included rename header (SuperEmbedderCore.Build.cs), so every engine
// link name here is this plugin's own and cannot meet the SuperSLM plugin's copy. The vendored file is not
// edited. SuperSLM v1.18.1, src/model.cpp.
#include "../../../ThirdParty/SuperSLM/src/model.cpp"
