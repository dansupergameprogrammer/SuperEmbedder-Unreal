// src/superembedder/json_lite.cpp -- see json_lite.h for scope.
#include "json_lite.h"

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <type_traits>

namespace semb::json {

namespace {

// TE-191 F1: the recursive-descent parser below has no depth or node
// budget of its own; a hostile header whose value is thousands of nested
// arrays recurses until the stack is exhausted (an unrecoverable process
// termination, not a thrown exception -- the outside review's executed
// probe exited with Windows status 0xC00000FD at depth 5,000; depth 1,000
// parsed). Both documents this reader serves (FORMAT.md sec 1's header,
// plan sec 3.4's manifest) nest at most a handful of levels deep in
// ordinary operation (an object containing a couple of arrays of scalars);
// kMaxJsonDepth leaves two orders of magnitude of headroom above that
// while sitting comfortably below the depth the executed probe showed
// unsafe.
//
// TE-193 P-6: kMaxJsonNodes bounds total parsed values -- and a bound on
// the COUNT alone does not bound MEMORY, because a wide-but-shallow input
// (many siblings rather than many nesting levels) is only refused at node
// kMaxJsonNodes + 1, by which point the tree the budget permits is already
// materialized: 20,000,006 integer siblings in a 40 MB header cost 3,328 MB
// of peak working set before the refusal arrived. The parser therefore
// refuses a wide container AT ITS OWN OPENING, before any sibling is
// materialized: CountDirectChildren() below scans the container's own span
// -- strings, escapes and nesting tracked -- at O(1) tree memory, and the
// container is refused up front when its direct children alone would
// exceed the remaining budget. The direct-child count is a lower bound on
// the subtree's node charge (every direct child is itself a parsed value),
// so the early refusal can never refuse a document the budget admits.
// A shallow hostile input is thus refused at memory proportional to its
// input; every other shape is bounded by the per-value charge at the
// ceiling's own cost, and nesting is bounded by the depth budget. The
// ceiling stays 20,000,000 total values -- well above any document this
// project bakes (a manifest's largest arrays are one entry per row) while
// remaining a finite, checkable ceiling.
constexpr int kMaxJsonDepth = 200;
constexpr size_t kMaxJsonNodes = 20'000'000;

void WriteEscapedString(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void WriteValue(const Value& v, std::string* out) {
  switch (v.kind) {
    case Kind::kNull:
      *out += "null";
      break;
    case Kind::kBool:
      *out += v.as_bool ? "true" : "false";
      break;
    case Kind::kInt:
      *out += std::to_string(v.as_int);
      break;
    case Kind::kString:
      WriteEscapedString(v.as_string, out);
      break;
    case Kind::kArray: {
      out->push_back('[');
      for (size_t i = 0; i < v.as_array.size(); ++i) {
        if (i) *out += ", ";
        WriteValue(v.as_array[i], out);
      }
      out->push_back(']');
      break;
    }
    case Kind::kObject: {
      out->push_back('{');
      for (size_t i = 0; i < v.as_object.size(); ++i) {
        if (i) *out += ", ";
        WriteEscapedString(v.as_object[i].first, out);
        *out += ": ";
        WriteValue(v.as_object[i].second, out);
      }
      out->push_back('}');
      break;
    }
  }
}

class ParserImpl {
 public:
  explicit ParserImpl(const std::string& text) : s_(text) {}

  Value Parse() {
    return ParseDocument<true>();
  }

  void Validate() {
    (void)ParseDocument<false>();
  }

 private:
  struct Validated {};
  template <bool Build>
  using Parsed = std::conditional_t<Build, Value, Validated>;

  template <bool Build>
  Parsed<Build> ParseDocument() {
    SkipWs();
    Parsed<Build> v = ParseValue<Build>();
    SkipWs();
    if (i_ != s_.size()) throw std::runtime_error("json_lite: trailing content after top-level value");
    return v;
  }

  const std::string& s_;
  size_t i_ = 0;
  int depth_ = 0;
  size_t node_count_ = 0;

  // TE-191 F1: charged once per parsed value (object, array, string, int,
  // bool, or null) -- the budget check every ParseValue() call pays before
  // doing anything else, so a hostile input is refused before it grows any
  // further rather than after it is fully materialized.
  void ChargeNode() {
    if (++node_count_ > kMaxJsonNodes) {
      throw std::runtime_error("json_lite: node budget exceeded");
    }
  }

  // RAII depth guard: entered on '{'/'[' before recursing into the
  // container's own values, left on return (including via an exception
  // unwinding through it) -- so depth_ always reflects the live recursion
  // depth regardless of which branch throws.
  struct DepthGuard {
    ParserImpl* p;
    explicit DepthGuard(ParserImpl* parser) : p(parser) {
      if (++p->depth_ > kMaxJsonDepth) {
        throw std::runtime_error("json_lite: nesting depth exceeded");
      }
    }
    ~DepthGuard() { --p->depth_; }
  };

  char Peek() const {
    if (i_ >= s_.size()) throw std::runtime_error("json_lite: unexpected end of input");
    return s_[i_];
  }
  char Next() {
    char c = Peek();
    ++i_;
    return c;
  }
  void SkipWs() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i_;
      } else {
        break;
      }
    }
  }
  void Expect(char c) {
    if (Next() != c) throw std::runtime_error(std::string("json_lite: expected '") + c + "'");
  }
  bool TryLiteral(const char* lit) {
    const size_t n = std::string(lit).size();
    if (s_.compare(i_, n, lit) == 0) {
      i_ += n;
      return true;
    }
    return false;
  }

  template <bool Build>
  Parsed<Build> ParseValue() {
    ChargeNode();
    SkipWs();
    const char c = Peek();
    if (c == '{') return ParseObject<Build>();
    if (c == '[') return ParseArray<Build>();
    if (c == '"') {
      std::string value = ParseString();
      if constexpr (Build) return Value::MakeString(std::move(value));
      else return {};
    }
    if (c == 't' || c == 'f') return ParseBool<Build>();
    if (c == 'n') {
      if (!TryLiteral("null")) throw std::runtime_error("json_lite: malformed literal");
      if constexpr (Build) return Value();
      else return {};
    }
    return ParseNumber<Build>();
  }

  // TE-193 P-6: counts the DIRECT children of the container whose opening
  // delimiter was just consumed, without materializing any of them -- a
  // string-, escape- and nesting-aware scan over the container's own span
  // (O(bytes) time, O(1) tree memory). Returns true with the child count in
  // *out when the container's matching close is found; returns false (no
  // verdict) when the input ends first or the closes mismatch -- that input
  // is malformed and the parser below refuses it anyway, with the per-value
  // charge still bounding the budget meanwhile.
  bool CountDirectChildren(char close, size_t* out) {
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t commas = 0;
    for (size_t j = i_; j < s_.size(); ++j) {
      const char c = s_[j];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '[' || c == '{') {
        ++depth;
      } else if (c == ']' || c == '}') {
        if (depth == 0) {
          if (c != close) return false;  // mismatched close: malformed, no verdict
          *out = commas + 1;
          return true;
        }
        --depth;
      } else if (depth == 0 && c == ',') {
        ++commas;
      }
    }
    return false;  // input ends before the container closes: malformed, no verdict
  }

  // TE-193 P-6: refuses, at the container's own opening and before any
  // sibling is materialized, a container whose direct children alone would
  // exceed the remaining node budget (see the kMaxJsonNodes comment above
  // for why the per-value charge alone cannot bound memory on this face).
  size_t RefuseWideContainer(char close) {
    size_t children = 0;
    if (CountDirectChildren(close, &children) && node_count_ + children > kMaxJsonNodes) {
      throw std::runtime_error("json_lite: node budget exceeded");
    }
    return children;
  }

  // Validate remembers a packed 12-byte record for every object member until
  // the closing brace, then sorts by the decoded-name hash. JSON's grammar
  // gives every member at least five bytes: the shortest string is `""` (two
  // bytes), followed by `:`, a one-digit value, and (except for the last
  // member) `,`. The steady retained cost is therefore 12 bytes per member,
  // below the 15 bytes per member that the 4x I3 slope permits.
  //
  // On the two-shared-name transient family, the live name-sized allocation
  // sites are enumerated here rather than inferred from variable names:
  // - bank.cpp:196-211,281-303 keeps the header read buffer alive; it is one
  //   header unit and contains the shared name twice.
  // - json_lite.cpp:320-330 decodes one name, and :345-356 has at most two
  //   such decoded strings live at either the comparator or equality branch.
  //   MSVC basic_string's _Calculate_growth
  //   (VC/Tools/MSVC/14.33.31629/include/xstring:4749-4760) caps each growing
  //   buffer below 1.5 times its requested name size, so they total below
  //   2 * 1.5 * (header / 2) = 1.5 header units.
  // - :379-399 scopes the parser-local key to its completed member. It is
  //   released before the duplicate check; :375-376 retain only two fixed-
  //   size records, and the fixed exception at :354 carries no member bytes.
  //   Those allocations are O(1), not name-sized.
  // Thus the source term is below 1 + 1.5 = 2.5 header units. TE-219 grades
  // the two-member family at 2.75 after its explicit 0.25 allocator margin.
  // Within an equal-hash run the records themselves -- not a materialized copy
  // of their names -- are sorted by decoded name. Exact decoded comparison
  // remains authoritative: the folded hash only sets average run length and
  // is never a bound or a duplicate verdict.
  static uint32_t HashDecodedMemberName(const std::string& name) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    (void)name;
    return 0;
#else
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : name) {
      hash ^= c;
      hash *= UINT64_C(1099511628211);
    }
    return static_cast<uint32_t>(hash ^ (hash >> 32));
#endif
  }

#pragma pack(push, 1)
  struct MemberNameRecord {
    uint32_t hash;
    size_t offset;
  };
#pragma pack(pop)

  static_assert(sizeof(MemberNameRecord) == 12, "member name records stay fixed at 12 bytes");

  std::string DecodeMemberNameAt(size_t offset) {
    const size_t saved = i_;
    i_ = offset;
    try {
      std::string decoded = ParseString();
      i_ = saved;
      return decoded;
    } catch (...) {
      i_ = saved;
      throw;
    }
  }

  void RememberMemberName(std::vector<MemberNameRecord>* records, const std::string& key, size_t key_offset) {
    records->push_back({HashDecodedMemberName(key), key_offset});
  }

  void RefuseDuplicateMemberNames(std::vector<MemberNameRecord>* records) {
    std::sort(records->begin(), records->end(), [](const MemberNameRecord& a, const MemberNameRecord& b) {
      return a.hash < b.hash;
    });
    for (size_t first = 0; first < records->size();) {
      size_t last = first + 1;
      while (last < records->size() && (*records)[last].hash == (*records)[first].hash) ++last;
      if (last - first > 1) {
        std::sort(records->begin() + static_cast<ptrdiff_t>(first),
                  records->begin() + static_cast<ptrdiff_t>(last),
                  [this](const MemberNameRecord& a, const MemberNameRecord& b) {
                    return DecodeMemberNameAt(a.offset) < DecodeMemberNameAt(b.offset);
                  });
        std::string previous = DecodeMemberNameAt((*records)[first].offset);
        for (size_t n = first + 1; n < last; ++n) {
          std::string current = DecodeMemberNameAt((*records)[n].offset);
          if (previous == current) {
            throw std::runtime_error("json_lite: duplicate object member");
          }
          previous = std::move(current);
        }
      }
      first = last;
    }
  }

  template <bool Build>
  Parsed<Build> ParseObject() {
    DepthGuard guard(this);
    Expect('{');
    SkipWs();
    if (Peek() == '}') {
      ++i_;
      Parsed<Build> empty;
      if constexpr (Build) empty = Value::MakeObject();
      return empty;
    }
    const size_t direct_members = RefuseWideContainer('}');
    std::vector<MemberNameRecord> member_name_records;
    member_name_records.reserve(direct_members);
    Parsed<Build> v;
    if constexpr (Build) v = Value::MakeObject();
    while (true) {
      SkipWs();
      const size_t key_offset = i_;
      {
        std::string key = ParseString();
        // JSON object member names are unique for every document this parser
        // accepts. Keeping the first occurrence would make a consumer disagree
        // with a last-member reader; reject the object before any caller can
        // look a field up through Value::Find().
        RememberMemberName(&member_name_records, key, key_offset);
        SkipWs();
        Expect(':');
        Parsed<Build> val = ParseValue<Build>();
        if constexpr (Build) v.as_object.emplace_back(std::move(key), std::move(val));
      }
      SkipWs();
      const char c = Next();
      if (c == ',') continue;
      if (c == '}') {
        RefuseDuplicateMemberNames(&member_name_records);
        break;
      }
      throw std::runtime_error("json_lite: malformed object");
    }
    return v;
  }

  template <bool Build>
  Parsed<Build> ParseArray() {
    DepthGuard guard(this);
    Expect('[');
    SkipWs();
    if (Peek() == ']') {
      ++i_;
      Parsed<Build> empty;
      if constexpr (Build) empty = Value::MakeArray();
      return empty;
    }
    RefuseWideContainer(']');
    Parsed<Build> v;
    if constexpr (Build) v = Value::MakeArray();
    while (true) {
      Parsed<Build> element = ParseValue<Build>();
      if constexpr (Build) v.as_array.push_back(std::move(element));
      SkipWs();
      const char c = Next();
      if (c == ',') continue;
      if (c == ']') break;
      throw std::runtime_error("json_lite: malformed array");
    }
    return v;
  }

  // Reads exactly four hex digits (a \u escape's own payload, with the
  // \u already consumed by the caller) and returns the code unit.
  unsigned int ParseHex4() {
    unsigned int cp = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = Next();
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
      else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
      else throw std::runtime_error("json_lite: malformed \\u escape");
    }
    return cp;
  }

  // Encodes one Unicode scalar value (BMP or astral -- never a lone
  // surrogate half, which ParseString's own \u handling below never hands
  // this function) as UTF-8.
  static void EncodeUtf8(unsigned int cp, std::string* out) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  std::string ParseString() {
    Expect('"');
    std::string out;
    while (true) {
      const char c = Next();
      if (c == '"') break;
      if (c == '\\') {
        const char e = Next();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            unsigned int cp = ParseHex4();
            // TE-191 F5: a lone \u escape was previously encoded as a
            // standalone BMP code point even when it was one half of a
            // surrogate pair -- the executed construction showed
            // "😀" (one Unicode scalar, U+1F600) and the
            // literal UTF-8 bytes for the same character decoding to
            // DIFFERENT byte sequences, so a semantic duplicate id
            // evaded the bytewise uniqueness check. A high surrogate
            // (0xD800-0xDBFF) must be immediately followed by a low
            // surrogate (0xDC00-0xDFFF) escape and the pair combines into
            // one astral code point; either half appearing alone is
            // malformed JSON and a parse failure (RFC 8259 sec 8.2 /
            // FORMAT.md sec 1's own "any non-finite value... duplicate or
            // wrongly-counted ids" obligations depend on strings decoding
            // to their one true scalar value).
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (Next() != '\\' || Next() != 'u') {
                throw std::runtime_error("json_lite: unpaired high surrogate");
              }
              const unsigned int low = ParseHex4();
              if (low < 0xDC00 || low > 0xDFFF) {
                throw std::runtime_error("json_lite: unpaired high surrogate");
              }
              cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              throw std::runtime_error("json_lite: unpaired low surrogate");
            }
            EncodeUtf8(cp, &out);
            break;
          }
          default:
            throw std::runtime_error("json_lite: malformed escape");
        }
      } else {
        // TE-191 F5: a raw, unescaped control byte inside a JSON string is
        // not valid JSON (RFC 8259 sec 7: "the characters that MUST be
        // escaped" includes U+0000 through U+001F unescaped) -- the
        // executed construction put a literal newline inside an id string
        // and open accepted it as SEMB_OK. Escaped forms (\n, \t, \uXXXX
        // below 0x20) remain accepted; only an unescaped raw byte in this
        // range is refused.
        if (static_cast<unsigned char>(c) < 0x20) {
          throw std::runtime_error("json_lite: raw control byte in string");
        }
        out.push_back(c);
      }
    }
    return out;
  }

  template <bool Build>
  Parsed<Build> ParseBool() {
    if (TryLiteral("true")) {
      if constexpr (Build) {
        Value v;
        v.kind = Kind::kBool;
        v.as_bool = true;
        return v;
      } else {
        return {};
      }
    }
    if (TryLiteral("false")) {
      if constexpr (Build) {
        Value v;
        v.kind = Kind::kBool;
        v.as_bool = false;
        return v;
      } else {
        return {};
      }
    }
    throw std::runtime_error("json_lite: malformed literal");
  }

  template <bool Build>
  Parsed<Build> ParseNumber() {
    const size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    bool any_digit = false;
    while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
      ++i_;
      any_digit = true;
    }
    if (!any_digit) throw std::runtime_error("json_lite: malformed number");
    // This library's own producer never writes a fractional/exponent
    // number (every numeric field in the two documents it serves is an
    // integer); a '.' or 'e'/'E' here means the input is not a shape this
    // reader supports.
    if (i_ < s_.size() && (s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) {
      throw std::runtime_error("json_lite: non-integer numbers are not supported");
    }
    // TE-193 P-14: std::stoll throws std::out_of_range (a std::logic_error)
    // on a digit string that does not fit int64 -- and this library's own
    // contract (json_lite.h) promises std::runtime_error on ANY malformed
    // input. An integer outside int64 is malformed for every shape this
    // reader serves (the two documents' numeric fields are all small
    // non-negative integers), and the only caller today maps whatever
    // escapes to SEMB_ARTIFACT_UNREADABLE via a std::exception catch --
    // but a future caller catching the DOCUMENTED type would not have
    // caught std::out_of_range. Translated here, so the documented type is
    // exactly what a caller can catch.
    int64_t parsed = 0;
    try {
      parsed = std::stoll(s_.substr(start, i_ - start));
    } catch (const std::out_of_range&) {
      throw std::runtime_error("json_lite: integer out of int64 range");
    }
    if constexpr (Build) return Value::MakeInt(parsed);
    else return {};
  }
};

}  // namespace

std::string Write(const Value& v) {
  std::string out;
  WriteValue(v, &out);
  return out;
}

Value Parse(const std::string& text) {
  ParserImpl p(text);
  return p.Parse();
}

void Validate(const std::string& text) {
  ParserImpl p(text);
  p.Validate();
}

}  // namespace semb::json
