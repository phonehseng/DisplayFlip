// Tiny JSON (with // and /* */ comments and trailing commas accepted on input).
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace df {

class Json {
 public:
  enum Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(bool b) : t_(Bool), b_(b) {}
  Json(int n) : t_(Number), n_(n) {}
  Json(unsigned n) : t_(Number), n_(n) {}
  Json(long long n) : t_(Number), n_((double)n) {}
  Json(unsigned long long n) : t_(Number), n_((double)n) {}
  Json(double n) : t_(Number), n_(n) {}
  Json(const char* s) : t_(String), s_(s) {}
  Json(const std::string& s) : t_(String), s_(s) {}
  Json(const std::wstring& s);
  static Json Arr() { Json j; j.t_ = Array; return j; }
  static Json Obj() { Json j; j.t_ = Object; return j; }

  Type type() const { return t_; }
  bool isNull() const { return t_ == Null; }
  bool isObj() const { return t_ == Object; }
  bool isArr() const { return t_ == Array; }
  bool isStr() const { return t_ == String; }
  bool isNum() const { return t_ == Number; }
  bool isBool() const { return t_ == Bool; }

  // accessors with defaults
  bool asBool(bool def = false) const { return t_ == Bool ? b_ : (t_ == Number ? n_ != 0 : def); }
  double asNum(double def = 0) const { return t_ == Number ? n_ : def; }
  long long asInt(long long def = 0) const { return t_ == Number ? (long long)n_ : def; }
  const std::string& asStr() const { return s_; }
  std::wstring asWStr(const std::wstring& def = L"") const;

  // object
  const Json& operator[](const char* key) const;
  Json& set(const std::string& key, const Json& v);
  bool has(const char* key) const;
  const std::vector<std::pair<std::string, Json>>& items() const { return o_; }
  std::wstring str(const char* key, const std::wstring& def = L"") const { return (*this)[key].asWStr(def); }
  double num(const char* key, double def = 0) const { const Json& j = (*this)[key]; return j.isNum() ? j.n_ : def; }
  bool boolean(const char* key, bool def = false) const { const Json& j = (*this)[key]; return j.isBool() || j.isNum() ? j.asBool() : def; }

  // array
  size_t size() const { return t_ == Array ? a_.size() : (t_ == Object ? o_.size() : 0); }
  const Json& at(size_t i) const;
  Json& push(const Json& v) { if (t_ != Array) { *this = Arr(); } a_.push_back(v); return a_.back(); }
  const std::vector<Json>& elements() const { return a_; }
  // string or array of strings -> list
  std::vector<std::wstring> strList() const;

  static Json Parse(const std::string& text, std::string* err = nullptr);
  std::string Dump(bool pretty = false, int indent = 0) const;

 private:
  Type t_ = Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Json> a_;
  std::vector<std::pair<std::string, Json>> o_;
  friend struct JsonParser;
};

}  // namespace df
