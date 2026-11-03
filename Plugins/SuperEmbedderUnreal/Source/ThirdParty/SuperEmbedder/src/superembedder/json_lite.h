// src/superembedder/json_lite.h -- TE-166 (U1 slice 4): a minimal,
// dependency-free JSON reader/writer for the bank pair's own
// `.wvbank.json` header (FORMAT.md sec 1) and the `.semb.json` manifest
// sidecar (plan sec 3.4). Product-side and internal only -- not part of
// the frozen public ABI, not installed, and never depends on
// `tests/json_mini.h` (product code does not depend on the test tree).
// Covers exactly the value shapes those two documents need: object,
// array, string, and int64 -- no float support, matching this project's
// own `tests/json_mini.h` precedent (every field either format parses is
// a string, an int, or an array/object of those).
#ifndef SUPEREMBEDDER_JSON_LITE_H
#define SUPEREMBEDDER_JSON_LITE_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace semb::json {

enum class Kind { kNull, kBool, kInt, kString, kArray, kObject };

// An object preserves insertion order (a vector of key/value pairs, not a
// map) so this library's own writer emits fields in the exact order the
// caller built them -- load-bearing for a manifest a human or another
// tool may diff. Lookup is linear, which is acceptable at this library's
// own scale (a few dozen fields, arrays of a few hundred entries).
struct Value {
  Kind kind = Kind::kNull;
  bool as_bool = false;
  int64_t as_int = 0;
  std::string as_string;
  std::vector<Value> as_array;
  std::vector<std::pair<std::string, Value>> as_object;

  static Value MakeString(std::string s) {
    Value v;
    v.kind = Kind::kString;
    v.as_string = std::move(s);
    return v;
  }
  static Value MakeInt(int64_t n) {
    Value v;
    v.kind = Kind::kInt;
    v.as_int = n;
    return v;
  }
  static Value MakeArray() {
    Value v;
    v.kind = Kind::kArray;
    return v;
  }
  static Value MakeObject() {
    Value v;
    v.kind = Kind::kObject;
    return v;
  }

  bool IsObject() const { return kind == Kind::kObject; }
  bool IsArray() const { return kind == Kind::kArray; }
  bool IsString() const { return kind == Kind::kString; }

  // Object helpers. Set() appends (or replaces an existing key's value in
  // place, keeping its original position) so a caller can build a manifest
  // field by field in the order sec 3.4 lists them.
  void Set(const std::string& key, Value v) {
    for (auto& kv : as_object) {
      if (kv.first == key) {
        kv.second = std::move(v);
        return;
      }
    }
    as_object.emplace_back(key, std::move(v));
  }
  bool Has(const std::string& key) const {
    for (const auto& kv : as_object) {
      if (kv.first == key) return true;
    }
    return false;
  }
  const Value* Find(const std::string& key) const {
    for (const auto& kv : as_object) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  const Value& At(const std::string& key) const {
    const Value* v = Find(key);
    if (!v) throw std::runtime_error("json_lite: missing key '" + key + "'");
    return *v;
  }
  const std::string& Str() const {
    if (kind != Kind::kString) throw std::runtime_error("json_lite: not a string");
    return as_string;
  }
  int64_t Int() const {
    if (kind != Kind::kInt) throw std::runtime_error("json_lite: not an int");
    return as_int;
  }
};

// Serializes `v` as compact JSON with a single space after every `:` and
// after every `,` -- the exact convention `tests/te163_bank_pair_test.cpp`'s
// own tamper constructions match against literally (e.g.
// `"\"schemaVersion\": 1"`, `"\"model_sha\": \"" + hex + "\""`).
std::string Write(const Value& v);

// Parses `text` as JSON. Throws std::runtime_error on any malformed input
// (a caller at a validation boundary maps that to the ABI's own
// SEMB_ARTIFACT_UNREADABLE / SEMB_MANIFEST_SCHEMA_UNSUPPORTED, never lets
// the exception escape the library). "Malformed" includes every input
// shape this reader refuses: bad structure, duplicate object members,
// non-integer numbers, raw control bytes, unpaired surrogates -- and an integer literal outside
// int64 range (translated from std::stoll's own std::out_of_range at the
// parse site, so the exception a caller catches is always the documented
// type).
Value Parse(const std::string& text);

// Validates exactly the JSON language and resource budgets Parse() accepts,
// without constructing a Value tree.  Validation boundaries that need only
// json_lite's refusal authority use this entry point before declining input.
void Validate(const std::string& text);

}  // namespace semb::json

#endif  // SUPEREMBEDDER_JSON_LITE_H
