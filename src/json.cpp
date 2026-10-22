#include "json.h"
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace df {

static const Json kNull;

Json::Json(const std::wstring& s) : t_(String), s_(Narrow(s)) {}

std::wstring Json::asWStr(const std::wstring& def) const {
  if (t_ == String) return Widen(s_);
  if (t_ == Number) {
    wchar_t b[64];
    if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) _snwprintf(b, 63, L"%lld", (long long)n_);
    else _snwprintf(b, 63, L"%g", n_);
    return b;
  }
  if (t_ == Bool) return b_ ? L"true" : L"false";
  return def;
}

const Json& Json::operator[](const char* key) const {
  if (t_ != Object) return kNull;
  for (auto& kv : o_)
    if (kv.first == key) return kv.second;
  // case-insensitive fallback so hand-edited configs are forgiving
  for (auto& kv : o_)
    if (_stricmp(kv.first.c_str(), key) == 0) return kv.second;
  return kNull;
}

bool Json::has(const char* key) const {
  if (t_ != Object) return false;
  for (auto& kv : o_)
    if (_stricmp(kv.first.c_str(), key) == 0) return true;
  return false;
}

Json& Json::set(const std::string& key, const Json& v) {
  if (t_ != Object) *this = Obj();
  for (auto& kv : o_)
    if (kv.first == key) { kv.second = v; return kv.second; }
  o_.emplace_back(key, v);
  return o_.back().second;
}

const Json& Json::at(size_t i) const { return (t_ == Array && i < a_.size()) ? a_[i] : kNull; }

std::vector<std::wstring> Json::strList() const {
  std::vector<std::wstring> r;
  if (t_ == String) { if (!s_.empty()) r.push_back(Widen(s_)); }
  else if (t_ == Array)
    for (auto& e : a_)
      if (e.isStr()) r.push_back(e.asWStr());
  return r;
}

// ---- parser ----------------------------------------------------------------
struct JsonParser {
  const char* p;
  const char* end;
  std::string err;

  void ws() {
    for (;;) {
      while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
      if (p + 1 < end && p[0] == '/' && p[1] == '/') {
        while (p < end && *p != '\n') p++;
        continue;
      }
      if (p + 1 < end && p[0] == '/' && p[1] == '*') {
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
        p = (p + 1 < end) ? p + 2 : end;
        continue;
      }
      break;
    }
  }

  bool fail(const char* m) {
    if (err.empty()) {
      char b[160];
      snprintf(b, sizeof b, "%s near: %.30s", m, p < end ? p : "<end>");
      err = b;
    }
    return false;
  }

  static void putUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  }

  bool str(std::string& o) {
    if (*p != '"') return fail("expected string");
    p++;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c != '\\') { o += c; continue; }
      if (p >= end) return fail("bad escape");
      char e = *p++;
      switch (e) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': o += '\r'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
          if (end - p < 4) return fail("bad \\u");
          unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            unsigned lo = (unsigned)strtoul(std::string(p + 2, 4).c_str(), nullptr, 16);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
          }
          putUtf8(o, cp);
          break;
        }
        default: o += e;  // \" \\ \/ and lenient for anything else (e.g. Windows paths)
      }
    }
    if (p >= end) return fail("unterminated string");
    p++;
    return true;
  }

  bool val(Json& v) {
    ws();
    if (p >= end) return fail("unexpected end");
    char c = *p;
    if (c == '{') {
      p++;
      v = Json::Obj();
      for (;;) {
        ws();
        if (p < end && *p == '}') { p++; return true; }
        std::string k;
        if (!str(k)) return false;
        ws();
        if (p >= end || *p != ':') return fail("expected ':'");
        p++;
        Json child;
        if (!val(child)) return false;
        v.o_.emplace_back(std::move(k), std::move(child));
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      p++;
      v = Json::Arr();
      for (;;) {
        ws();
        if (p < end && *p == ']') { p++; return true; }
        Json child;
        if (!val(child)) return false;
        v.a_.push_back(std::move(child));
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      v = Json("");
      return str(v.s_);
    }
    if (!strncmp(p, "true", 4)) { p += 4; v = Json(true); return true; }
    if (!strncmp(p, "false", 5)) { p += 5; v = Json(false); return true; }
    if (!strncmp(p, "null", 4)) { p += 4; v = Json(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      char* e = nullptr;
      double d = strtod(p, &e);
      if (e == p) return fail("bad number");
      p = e;
      v = Json(d);
      return true;
    }
    return fail("unexpected character");
  }
};

Json Json::Parse(const std::string& text, std::string* err) {
  JsonParser ps{text.data(), text.data() + text.size(), {}};
  Json v;
  if (!ps.val(v)) {
    if (err) *err = ps.err;
    return Json();
  }
  if (err) err->clear();
  return v;
}

static void dumpStr(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  o += '"';
}

std::string Json::Dump(bool pretty, int indent) const {
  std::string o;
  std::string pad = pretty ? std::string((indent + 1) * 2, ' ') : "";
  std::string padEnd = pretty ? std::string(indent * 2, ' ') : "";
  const char* nl = pretty ? "\n" : "";
  switch (t_) {
    case Null: return "null";
    case Bool: return b_ ? "true" : "false";
    case Number: {
      char b[64];
      if (n_ == std::floor(n_) && std::fabs(n_) < 9.0e15) snprintf(b, 64, "%lld", (long long)n_);
      else snprintf(b, 64, "%.10g", n_);
      return b;
    }
    case String: dumpStr(o, s_); return o;
    case Array: {
      if (a_.empty()) return "[]";
      // short scalar arrays stay on one line
      bool flat = true;
      for (auto& e : a_) if (e.t_ == Array || e.t_ == Object) flat = false;
      o += '[';
      for (size_t i = 0; i < a_.size(); i++) {
        if (i) o += flat || !pretty ? (pretty ? ", " : ",") : ",";
        if (!flat) { o += nl; o += pad; }
        o += a_[i].Dump(pretty, indent + 1);
      }
      if (!flat) { o += nl; o += padEnd; }
      o += ']';
      return o;
    }
    case Object: {
      if (o_.empty()) return "{}";
      o += '{';
      for (size_t i = 0; i < o_.size(); i++) {
        if (i) o += ',';
        o += nl;
        o += pad;
        dumpStr(o, o_[i].first);
        o += pretty ? ": " : ":";
        o += o_[i].second.Dump(pretty, indent + 1);
      }
      o += nl;
      o += padEnd;
      o += '}';
      return o;
    }
  }
  return o;
}

}  // namespace df
