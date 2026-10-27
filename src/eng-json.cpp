// eng-json.cpp  -  recursive-descent parser for the JSON subset the bakes use
// (they are node-emitted canonical JSON, so this can be small and strict enough).
#include "eng-json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace eng {

namespace {

struct Parser {
  const char* p;
  const char* end;
  const char* start;
  std::string err;
  bool failed;

  explicit Parser(const std::string& text)
      : p(text.c_str()), end(text.c_str() + text.size()), start(text.c_str()), failed(false) {}

  void skipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  bool fail(const char* msg) {
    if (!failed) {
      err = std::string(msg) + " at byte " + std::to_string((long)(p - start));
      failed = true;
    }
    return false;
  }

  bool parseValue(JsonValue& out) {
    skipWs();
    if (p >= end) return fail("unexpected end of input");
    const char c = *p;
    if (c == '{') return parseObject(out);
    if (c == '[') return parseArray(out);
    if (c == '"') { out.type = JsonValue::String; return parseString(out.str); }
    if (c == 't') { out.type = JsonValue::Bool; out.b = true; return accept("true"); }
    if (c == 'f') { out.type = JsonValue::Bool; out.b = false; return accept("false"); }
    if (c == 'n') { out.type = JsonValue::Null; return accept("null"); }
    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
    return fail("unexpected character");
  }

  bool accept(const char* word) {
    const size_t n = strlen(word);
    if (end - p >= (long)n && strncmp(p, word, n) == 0) {
      p += n;
      return true;
    }
    return fail("bad literal");
  }

  bool parseNumber(JsonValue& out) {
    char buf[64];
    int n = 0;
    while (p < end && n < 63 && (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) {
      buf[n++] = *p++;
    }
    buf[n] = 0;
    char* stop = 0;
    out.type = JsonValue::Number;
    out.num = strtod(buf, &stop);
    if (stop != buf + n) return fail("bad number");
    return true;
  }

  void utf8Append(unsigned int cp, std::string& s) {
    if (cp < 0x80) {
      s.push_back((char)cp);
    } else if (cp < 0x800) {
      s.push_back((char)(0xC0 | (cp >> 6)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back((char)(0xE0 | (cp >> 12)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      s.push_back((char)(0xF0 | (cp >> 18)));
      s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }

  bool parseString(std::string& out) {
    if (p >= end || *p != '"') return fail("expected string");
    p++;
    out.clear();
    while (p < end && *p != '"') {
      if (*p == '\\') {
        p++;
        if (p >= end) return fail("unterminated escape");
        switch (*p) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (end - p < 5) return fail("short \\u escape");
            unsigned int cp = 0;
            for (int i = 1; i <= 4; i++) {
              char h = p[i];
              unsigned int v;
              if (h >= '0' && h <= '9') v = (unsigned int)(h - '0');
              else if (h >= 'a' && h <= 'f') v = (unsigned int)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') v = (unsigned int)(h - 'A' + 10);
              else return fail("bad hex in \\u escape");
              cp = cp * 16 + v;
            }
            p += 4;
            utf8Append(cp, out);
            break;
          }
          default: return fail("bad escape");
        }
        p++;
      } else {
        const char* run = p;                     // a run of plain characters, appended at once
        while (p < end && *p != '"' && *p != '\\') p++;
        out.append(run, (size_t)(p - run));
      }
    }
    if (p >= end) return fail("unterminated string");
    p++; // closing quote
    return true;
  }

  bool parseArray(JsonValue& out) {
    p++; // [
    out.type = JsonValue::Array;
    skipWs();
    if (p < end && *p == ']') { p++; return true; }
    for (;;) {
      // Parse straight into the new slot. `push_back(v)` of a parsed temporary deep-copied every
      // subtree once per nesting level above it: the 3.3 MB School bake took 481 ms (7 MB/s).
      out.arr.push_back(JsonValue());
      if (!parseValue(out.arr.back())) return false;
      skipWs();
      if (p < end && *p == ',') { p++; continue; }
      if (p < end && *p == ']') { p++; return true; }
      return fail("expected , or ]");
    }
  }

  bool parseObject(JsonValue& out) {
    p++; // {
    out.type = JsonValue::Object;
    skipWs();
    if (p < end && *p == '}') { p++; return true; }
    for (;;) {
      skipWs();
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (p >= end || *p != ':') return fail("expected :");
      p++;
      out.obj.push_back(std::make_pair(std::string(), JsonValue()));
      out.obj.back().first.swap(key);
      if (!parseValue(out.obj.back().second)) return false;   // in place (see parseArray)
      skipWs();
      if (p < end && *p == ',') { p++; continue; }
      if (p < end && *p == '}') { p++; return true; }
      return fail("expected , or }");
    }
  }
};

} // namespace

bool jsonParse(const std::string& text, JsonValue& out, std::string& err) {
  Parser parser(text);
  out = JsonValue();
  if (!parser.parseValue(out)) {
    err = parser.err.empty() ? "parse error" : parser.err;
    return false;
  }
  return true;
}

#ifdef _MSC_VER
static FILE* jfopen(const char* path, const char* mode) {
  FILE* f = 0;
  if (fopen_s(&f, path, mode) != 0) return 0;
  return f;
}
#else
static FILE* jfopen(const char* path, const char* mode) { return fopen(path, mode); }
#endif

std::string jsonReadFile(const std::string& path, bool& ok) {
  ok = false;
  FILE* f = jfopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string data;
  char buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, got);
  fclose(f);
  ok = true;
  return data;
}

} // namespace eng
