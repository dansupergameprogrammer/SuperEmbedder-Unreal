// src/c12b/c12b_row_digest_main.cpp -- C12b's instrument
// (`Claude/Plans/SuperEmbedder_Plan.md` sec 8, C12b; D-SLM4648): ONE
// instrument over ONE fixed corpus on ONE artifact, emitting PER-ROW SHA-256
// over the emitted `{codes, m, e}` seam bytes -- never only an aggregate.
//
// It calls the library's own entry points (include/superembedder/
// encoder_interim_abi.h, seam_abi.h) and performs no arithmetic of its own on the
// reproducible path: it is a loop, a serialiser and a digest -- the same
// posture as the witness executable (sec 4.4b's structural law, clause 4).
// The digest is the engine's own portable SHA-256 (superslm/sha256.h), one
// hash function for every row here as for every fingerprint field.
//
// PER ROW, THREE DIGESTS -- so a red reading attributes by investigation:
//   seam_sha256          the whole document's seam (every position, position
//                        0 and the appended end token included, pre-final-
//                        norm), serialised by semb_seam_serialize_document.
//   psn_selected_sha256  the ONE position PS-N selects (the last, or
//                        --select second-last for the must-reject), AFTER the
//                        engine's final norm, serialised the same way as a
//                        one-position document.
//   psn_vector_sha256    the float32 vector PS-N emits from it, as
//                        little-endian bit patterns -- what a user receives.
//
// --leg chunk|token runs the same document down either installed forward
// entry point (C12b's "entry-point substitution" attribution, decided per
// row by the caller comparing two runs). --select last|second-last is the
// PS-N wrong-position MUST-REJECT's knob (sec 9 dim 10), reached through
// the library's own real selection verb, never a mutation of it.
//
// The product's COMPOSED verb (semb_interim_encode_ps_n) is run beside the
// decomposed path on every `--select last` row, and this instrument refuses
// (exit 3) if the two vectors ever disagree -- a composition that silently
// selects a different position than its parts would be exactly the port
// defect C12b exists to attribute.
//
// USAGE
//   semb_c12b_row_digest --model <path.sslm> --corpus <tokens.tsv> --out <rows.tsv>
//       [--leg chunk|token] [--chunk-budget N] [--select last|second-last]
//       [--append-token-id ID] [--dump-selected <path.tsv>] [--limit N]
//       [--print-vector <label>]
//   tokens.tsv rows: <label>\t<comma-separated content token ids>
//
// Exit codes: 0 every row encoded; 1 an engine refusal (named on stderr);
// 2 usage; 3 the composed verb disagrees with the decomposed path.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "superembedder/encoder_interim_abi.h"
#include "superembedder/seam_abi.h"
#include "superslm/sha256.h"

namespace {

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --model <path.sslm> --corpus <tokens.tsv> --out <rows.tsv> "
               "[--leg chunk|token] [--chunk-budget N] [--select last|second-last] "
               "[--append-token-id ID] [--dump-selected <path.tsv>] [--limit N] "
               "[--print-vector <label>]\n",
               argv0);
}

struct Document {
  std::string label;
  std::vector<int32_t> content_ids;
};

bool ReadCorpus(const std::string& path, std::vector<Document>* out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) return false;
    Document d;
    d.label = line.substr(0, tab);
    const std::string csv = line.substr(tab + 1);
    size_t start = 0;
    while (start <= csv.size()) {
      size_t comma = csv.find(',', start);
      if (comma == std::string::npos) comma = csv.size();
      if (comma > start) d.content_ids.push_back(std::atoi(csv.substr(start, comma - start).c_str()));
      start = comma + 1;
    }
    out->push_back(std::move(d));
  }
  return true;
}

std::string HexOfBytes(const std::vector<uint8_t>& bytes) {
  uint8_t digest[32];
  superslm::Sha256Hash(bytes.data(), bytes.size(), digest);
  return superslm::ToHex(digest);
}

// Serialises one document's seam through the library's own verb and digests it.
std::string SeamDigest(const int8_t* codes, const int64_t* m, const int64_t* e, size_t n,
                       size_t hidden, size_t* out_len) {
  std::vector<uint8_t> buf(16 + n * (hidden + 16));
  size_t len = 0;
  const SembSerializeStatus st =
      semb_seam_serialize_document(codes, m, e, n, hidden, buf.data(), buf.size(), &len);
  if (st != kSembSerializeOk) {
    std::fprintf(stderr, "c12b: serialize returned status %d\n", static_cast<int>(st));
    std::exit(1);
  }
  buf.resize(len);
  if (out_len) *out_len = len;
  return HexOfBytes(buf);
}

// The float32 vector as little-endian bit patterns, platform-invariant.
std::string VectorDigest(const std::vector<float>& v, std::vector<uint32_t>* out_bits) {
  std::vector<uint8_t> bytes(v.size() * 4);
  out_bits->resize(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    uint32_t bits;
    std::memcpy(&bits, &v[i], 4);
    (*out_bits)[i] = bits;
    for (int b = 0; b < 4; ++b) bytes[i * 4 + b] = static_cast<uint8_t>((bits >> (8 * b)) & 0xFF);
  }
  return HexOfBytes(bytes);
}

const char* PoolStatusName(SembWaPoolStatus s) {
  switch (s) {
    case kSembOk: return "SEMB_OK";
    case kSembInputTooShort: return "SEMB_INPUT_TOO_SHORT";
    case kSembAccumulatorOverflow: return "SEMB_ACCUMULATOR_OVERFLOW";
    case kSembPooledSubnormal: return "SEMB_POOLED_SUBNORMAL";
    case kSembPooledOverflow: return "SEMB_POOLED_OVERFLOW";
    case kSembEMinDomainExceeded: return "SEMB_E_MIN_DOMAIN_EXCEEDED";
  }
  return "SEMB_UNKNOWN";
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path, corpus_path, out_path, dump_path, print_vector_label;
  std::string leg_name = "chunk", select_name = "last";
  size_t chunk_budget = 0;
  size_t limit = 0;
  int32_t append_id = kSembPsNAppendedTokenId;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string* dst) {
      if (i + 1 >= argc) { PrintUsage(argv[0]); std::exit(2); }
      *dst = argv[++i];
    };
    std::string v;
    if (a == "--model") next(&model_path);
    else if (a == "--corpus") next(&corpus_path);
    else if (a == "--out") next(&out_path);
    else if (a == "--leg") next(&leg_name);
    else if (a == "--select") next(&select_name);
    else if (a == "--dump-selected") next(&dump_path);
    else if (a == "--print-vector") next(&print_vector_label);
    else if (a == "--chunk-budget") { next(&v); chunk_budget = static_cast<size_t>(std::atoll(v.c_str())); }
    else if (a == "--limit") { next(&v); limit = static_cast<size_t>(std::atoll(v.c_str())); }
    else if (a == "--append-token-id") { next(&v); append_id = std::atoi(v.c_str()); }
    else { PrintUsage(argv[0]); return 2; }
  }
  if (model_path.empty() || corpus_path.empty() || out_path.empty()) { PrintUsage(argv[0]); return 2; }
  SembForwardLeg leg;
  if (leg_name == "chunk") leg = kSembForwardLegChunkBatched;
  else if (leg_name == "token") leg = kSembForwardLegPerToken;
  else { PrintUsage(argv[0]); return 2; }
  if (select_name != "last" && select_name != "second-last") { PrintUsage(argv[0]); return 2; }
  const bool select_last = select_name == "last";

  std::vector<Document> docs;
  if (!ReadCorpus(corpus_path, &docs) || docs.empty()) {
    std::fprintf(stderr, "c12b: could not read a non-empty corpus from %s\n", corpus_path.c_str());
    return 2;
  }
  if (limit > 0 && docs.size() > limit) docs.resize(limit);

  const auto t_open0 = std::chrono::steady_clock::now();
  SembInterimEncoder* enc = nullptr;
  const char* engine_name = nullptr;
  SembInterimStatus st = semb_interim_encoder_open(model_path.c_str(), append_id, &enc, &engine_name);
  if (st != kSembInterimOk) {
    std::fprintf(stderr, "c12b: SEMB_ENGINE_REFUSED at open (status=%d, engine=%s)\n",
                 static_cast<int>(st), engine_name ? engine_name : "?");
    return 1;
  }
  const double open_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_open0).count();
  SembInterimGeometry g{};
  g.struct_size = sizeof(g);
  if (semb_interim_encoder_geometry(enc, &g) != kSembInterimOk) {
    std::fprintf(stderr, "c12b: geometry query refused\n");
    return 1;
  }
  const size_t hidden = g.hidden_size;
  std::printf("c12b: model=%s hidden=%u layers=%u heads=%u kv_heads=%u head_dim=%u vocab=%u "
              "context_cap=%u appended_token_id=%d open_ms=%.1f leg=%s chunk_budget=%zu select=%s "
              "rows=%zu\n",
              model_path.c_str(), g.hidden_size, g.num_hidden_layers, g.num_attention_heads,
              g.num_key_value_heads, g.head_dim, g.vocab_size, g.context_cap, g.appended_token_id,
              open_ms, leg_name.c_str(), chunk_budget, select_name.c_str(), docs.size());

  std::ofstream out(out_path, std::ios::binary);
  if (!out) { std::fprintf(stderr, "c12b: cannot open --out %s\n", out_path.c_str()); return 2; }
  out << "# semb_c12b_row_digest rows: label\tn_positions\tseam_sha256\tpsn_selected_sha256\t"
         "psn_vector_sha256\tencode_ms\n";
  std::ofstream dump;
  if (!dump_path.empty()) {
    dump.open(dump_path, std::ios::binary);
    if (!dump) { std::fprintf(stderr, "c12b: cannot open --dump-selected %s\n", dump_path.c_str()); return 2; }
    dump << "# label\tstatus\tm\te\tcodes\tbits\n";
  }

  double total_encode_ms = 0.0;
  size_t total_positions = 0;
  size_t dequantize_refusals = 0;
  for (const Document& d : docs) {
    const size_t n = d.content_ids.size() + 1;
    std::vector<int8_t> codes(n * hidden);
    std::vector<int64_t> m(n), e(n);
    size_t positions = 0;
    const auto t0 = std::chrono::steady_clock::now();
    st = semb_interim_encode_seam(enc, d.content_ids.data(), d.content_ids.size(), chunk_budget,
                                  leg, codes.data(), codes.size(), m.data(), e.data(), n,
                                  &positions, &engine_name);
    const double encode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (st != kSembInterimOk) {
      std::fprintf(stderr, "c12b: %s: SEMB_ENGINE_REFUSED at encode (status=%d, engine=%s)\n",
                   d.label.c_str(), static_cast<int>(st), engine_name ? engine_name : "?");
      return 1;
    }
    if (positions != n) {
      std::fprintf(stderr, "c12b: %s: positions=%zu, expected %zu\n", d.label.c_str(), positions, n);
      return 1;
    }
    total_encode_ms += encode_ms;
    total_positions += n;

    size_t seam_len = 0;
    const std::string seam_hex = SeamDigest(codes.data(), m.data(), e.data(), n, hidden, &seam_len);

    const size_t position = select_last ? n - 1 : n - 2;  // n >= 2 always (content >= 1 + end token)
    std::vector<int8_t> sel(hidden);
    int64_t sel_m = 0, sel_e = 0;
    st = semb_interim_ps_n_select(enc, codes.data(), m.data(), e.data(), n, position, sel.data(),
                                  &sel_m, &sel_e, &engine_name);
    if (st != kSembInterimOk) {
      std::fprintf(stderr, "c12b: %s: SEMB_ENGINE_REFUSED at select (status=%d, engine=%s)\n",
                   d.label.c_str(), static_cast<int>(st), engine_name ? engine_name : "?");
      return 1;
    }
    const std::string sel_hex = SeamDigest(sel.data(), &sel_m, &sel_e, 1, hidden, nullptr);

    std::vector<float> vec(hidden, 0.0f);
    const SembWaPoolStatus dq = semb_ps_n_dequantize(sel.data(), sel_m, sel_e, hidden, vec.data());
    std::vector<uint32_t> bits;
    std::string vec_hex;
    if (dq == kSembOk) {
      vec_hex = VectorDigest(vec, &bits);
    } else {
      // A refusal is a reading, digested by NAME so a row that refuses
      // differs from every row that emits (sec 4.4's own discipline).
      ++dequantize_refusals;
      const std::string name = std::string("REFUSED:") + PoolStatusName(dq);
      vec_hex = HexOfBytes(std::vector<uint8_t>(name.begin(), name.end()));
    }

    // The composed product path must agree with the decomposed one, bit for bit.
    if (select_last) {
      std::vector<float> composed(hidden, 0.0f);
      size_t required = 0;
      SembWaPoolStatus dq2 = kSembOk;
      const SembInterimStatus cst = semb_interim_encode_ps_n(
          enc, d.content_ids.data(), d.content_ids.size(), chunk_budget, composed.data(),
          composed.size() * sizeof(float), &required, &dq2, &engine_name);
      const bool agree = (dq == kSembOk)
                             ? (cst == kSembInterimOk && required == hidden * sizeof(float) &&
                                std::memcmp(composed.data(), vec.data(), hidden * sizeof(float)) == 0)
                             : (cst == kSembInterimDequantizeRefused && dq2 == dq);
      if (!agree) {
        std::fprintf(stderr,
                     "c12b: %s: SEMB_C12B_COMPOSITION_DISAGREES -- semb_interim_encode_ps_n "
                     "(status=%d, dequantize=%s) does not reproduce select(n-1)+dequantize "
                     "(dequantize=%s)\n",
                     d.label.c_str(), static_cast<int>(cst), PoolStatusName(dq2), PoolStatusName(dq));
        return 3;
      }
    }

    out << d.label << '\t' << n << '\t' << seam_hex << '\t' << sel_hex << '\t' << vec_hex << '\t';
    char ms_buf[32];
    std::snprintf(ms_buf, sizeof(ms_buf), "%.3f", encode_ms);
    out << ms_buf << '\n';

    if (dump.is_open()) {
      dump << d.label << '\t' << PoolStatusName(dq) << '\t' << sel_m << '\t' << sel_e << '\t';
      for (size_t i = 0; i < hidden; ++i) dump << (i ? "," : "") << static_cast<int>(sel[i]);
      dump << '\t';
      if (dq == kSembOk) {
        char hb[16];
        for (size_t i = 0; i < hidden; ++i) {
          std::snprintf(hb, sizeof(hb), "%08x", bits[i]);
          dump << (i ? "," : "") << hb;
        }
      }
      dump << '\n';
    }

    if (!print_vector_label.empty() && d.label == print_vector_label) {
      std::printf("c12b: vector for %s: n=%zu position=%zu dequantize=%s dims=%zu first8=",
                  d.label.c_str(), n, position, PoolStatusName(dq), hidden);
      for (size_t i = 0; i < hidden && i < 8; ++i) std::printf("%s%.9g", i ? "," : "", vec[i]);
      std::printf(" seam_sha256=%s psn_vector_sha256=%s\n", seam_hex.c_str(), vec_hex.c_str());
    }
  }

  std::printf("c12b: rows=%zu positions=%zu encode_total_ms=%.1f encode_ms_per_position=%.3f "
              "dequantize_refusals=%zu leg=%s chunk_budget=%zu select=%s\n",
              docs.size(), total_positions, total_encode_ms,
              total_positions ? total_encode_ms / static_cast<double>(total_positions) : 0.0,
              dequantize_refusals, leg_name.c_str(), chunk_budget, select_name.c_str());
  semb_interim_encoder_close(enc);
  return 0;
}
