// The semb.h smoke, shared by the coexist binary and the plain reference binary. Encodes the synthetic
// texts as documents (semb_encode) and as queries (semb_encode_query) and prints each vector's digest:
// SHA-256 of its float32 values in little-endian order (the proposal's section 10.1 vector digest).
#include <array>
#include <string>
namespace semb_host_smoke {
// A self-contained SHA-256, so the digest does not depend on either engine copy.
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  static uint32_t R(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i)
      w[i] = w[i - 16] + (R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
             (R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10));
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string Hex(const uint8_t* data, size_t n) {
    std::string m(reinterpret_cast<const char*>(data), n);
    const uint64_t bits = uint64_t(n) * 8;
    m.push_back(char(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(char(bits >> (8 * i)));
    for (size_t i = 0; i < m.size(); i += 64) Block(reinterpret_cast<const uint8_t*>(m.data()) + i);
    static const char* x = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h) for (int s = 28; s >= 0; s -= 4) out.push_back(x[(v >> s) & 15]);
    return out;
  }
};
}  // namespace semb_host_smoke

int SmokeEncode(const char* model, const char* tokenizer) {
  semb_config cfg{};
  cfg.struct_size = sizeof(cfg);
  cfg.model_path = model;
  cfg.model_path_len = std::strlen(model);
  cfg.tokenizer_path = tokenizer;
  cfg.tokenizer_path_len = std::strlen(tokenizer);
  semb_encoder enc = nullptr;
  semb_status s = semb_encoder_open(&cfg, &enc);
  std::printf("semb_encoder_open -> %s\n", semb_status_name(s));
  if (s != SEMB_OK) return 1;
  uint32_t dims = 0;
  semb_encoder_dims(enc, &dims);
  std::printf("dims %u\n", dims);
  static const char* const kTexts[] = {"the drowned monastery", "a lantern that never goes out",
                                       "unicode caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"};
  int fail = 0;
  for (int kind = 0; kind < 2; ++kind) {
    for (size_t t = 0; t < sizeof(kTexts) / sizeof(kTexts[0]); ++t) {
      std::vector<float> v(dims + 4);
      float* aligned = reinterpret_cast<float*>((reinterpret_cast<uintptr_t>(v.data()) + 15) & ~uintptr_t(15));
      size_t required = 0;
      semb_fingerprint fp{};
      fp.struct_size = sizeof(fp);
      s = (kind == 0 ? semb_encode : semb_encode_query)(enc, kTexts[t], std::strlen(kTexts[t]), aligned,
                                                        dims * sizeof(float), &required, &fp);
      if (s != SEMB_OK) {
        std::printf("encode %s %zu -> %s\n", kind ? "query" : "document", t, semb_status_name(s));
        ++fail;
        continue;
      }
      std::vector<uint8_t> le(dims * 4);
      for (uint32_t i = 0; i < dims; ++i) {
        uint32_t u;
        std::memcpy(&u, &aligned[i], 4);
        for (int b = 0; b < 4; ++b) le[4 * i + b] = uint8_t(u >> (8 * b));
      }
      semb_host_smoke::Sha256 sha;
      std::printf("digest %s %zu %s\n", kind ? "query" : "document", t, sha.Hex(le.data(), le.size()).c_str());
      if (kind == 0 && t == 0) {
        semb_host_smoke::Sha256 fsha;
        std::printf("fingerprint %s\n", fsha.Hex(fp.digest, 32).c_str());
      }
    }
  }
  semb_encoder_close(enc);
  return fail;
}
