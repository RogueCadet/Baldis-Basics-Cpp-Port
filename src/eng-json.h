// eng-json.h  -  minimal JSON reader for the bake binding tables (no third-party deps; iOS-safe).
// Objects keep their FILE ORDER (std::vector of pairs)  -  Unity scene doc order drives
// instantiation, so order-preserving access is part of the parity contract.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>   // strtoll (exact fileID parsing)

namespace eng {

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object };

  Type type = Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue> > obj; // insertion order preserved

  const JsonValue* find(const char* key) const {
    for (size_t i = 0; i < obj.size(); i++) {
      if (obj[i].first == key) return &obj[i].second;
    }
    return 0;
  }
  bool has(const char* key) const { return find(key) != 0; }
  bool isNull() const { return type == Null; }
  double numberOr(double def) const { return type == Number ? num : def; }
  int asInt(int def = 0) const { return (int)numberOr((double)def); }
  float asFloat(float def = 0.0f) const { return (float)numberOr((double)def); }
  bool asBool(bool def = false) const {
    if (type == Bool) return b;
    if (type == Number) return num != 0.0;
    return def;
  }
  std::string asString() const { return type == String ? str : std::string(); } // null -> ""
  int size() const { return type == Array ? (int)arr.size() : 0; }
  const JsonValue* at(size_t i) const { return i < arr.size() ? &arr[i] : 0; }

  // Unity fileID as an exact 64-bit value.
  //
  // fileIDs run past 2^53 (prefab ids reach ~9.06e18, and MainMenu.unity carries 192 of them), so
  // they CANNOT round-trip through `num` (a double) -- distinct ids would collapse onto one value.
  // The baker therefore emits them as decimal STRINGS. Numbers stay accepted so a small-id bake
  // produced before that change still loads; `0` is Unity's null reference either way.
  long long asFileId(long long def = 0) const {
    if (type == String) return str.empty() ? def : strtoll(str.c_str(), 0, 10);
    if (type == Number) return (long long)num;
    return def;
  }
};

// fileID of `obj[key]`, or 0 when the key is absent / not an id. 0 == Unity's null reference.
inline long long fileIdOf(const JsonValue& obj, const char* key) {
  const JsonValue* v = obj.find(key);
  return v ? v->asFileId(0) : 0;
}

// Parses `text` into `out`. Returns false and fills `err` on malformed input.
bool jsonParse(const std::string& text, JsonValue& out, std::string& err);

// Convenience: read the whole file (fopenSafe under MSVC, plain fopen elsewhere).
std::string jsonReadFile(const std::string& path, bool& ok);

} // namespace eng
