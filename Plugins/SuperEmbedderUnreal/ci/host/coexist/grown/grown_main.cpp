// Grown-struct fixture program: the SuperSLM plugin's plain engine and the grown, renamed engine linked into
// one image, as a monolithic game links the two plugins. Runs both copies on the two-schema model.
//   grown <two_schema_model.sslm>
#include <cstdio>
#include <cstddef>
#include <vector>
extern "C" int drv_plugin(const void*, size_t, const char*);
extern "C" int drv_embedder(const void*, size_t, const char*);
int main(int argc, char** argv) {
	if (argc != 2) { std::fprintf(stderr, "usage: grown <two_schema_model.sslm>\n"); return 2; }
	std::vector<unsigned char> d;
	if (FILE* f = std::fopen(argv[1], "rb")) {
		int c;
		while ((c = std::fgetc(f)) != EOF) d.push_back(static_cast<unsigned char>(c));
		std::fclose(f);
	}
	if (d.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
	const int a = drv_plugin(d.data(), d.size(), "SuperSLM plugin, plain");
	const int b = drv_embedder(d.data(), d.size(), "SuperEmbedderCore, grown and renamed");
	std::printf("grown: plain=%d renamed=%d -> %s\n", a, b, (a || b) ? "FAIL" : "PASS");
	return (a || b) ? 1 : 0;
}
