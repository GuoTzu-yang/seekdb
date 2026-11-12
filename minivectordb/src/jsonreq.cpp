#include "jsonreq.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <set>

namespace mvdb {
namespace {

void utf8_append(std::string& s, uint32_t cp) {
  if (cp <= 0x7F) {
    s.push_back((char)cp);
  } else if (cp <= 0x7FF) {
    s.push_back((char)(0xC0 | (cp >> 6)));
    s.push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
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

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
      ++i_;
  }

  bool eof() { ws(); return i_ >= s_.size(); }

  char peek() { ws(); return i_ < s_.size() ? s_[i_] : '\0'; }

  bool eat(char c) {
    ws();
    if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
    return false;
  }

  // 解析 JSON 字符串（调用时 s_[i_] == '"'）
  bool str(std::string& out) {
    ws();
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    out.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') { out.push_back(c); continue; }
      if (i_ >= s_.size()) return false;
      char e = s_[i_++];
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
          if (i_ + 4 > s_.size()) return false;
          uint32_t cp = 0;
          for (int k = 0; k < 4; ++k) {
            int v = hex_val(s_[i_ + k]);
            if (v < 0) return false;
            cp = cp * 16 + (uint32_t)v;
          }
          i_ += 4;
          // UTF-16 代理对
          if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() &&
              s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            uint32_t lo = 0;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
              int v = hex_val(s_[i_ + 2 + k]);
              if (v < 0) { ok = false; break; }
              lo = lo * 16 + (uint32_t)v;
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
              i_ += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
          }
          utf8_append(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool integer(long long& out) {
    ws();
    size_t b = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    size_t digits = i_;
    while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) ++i_;
    if (i_ == digits) { i_ = b; return false; }
    // 关键：后面必须是分隔符才算「整数」。否则像 1.0 / 1e0 / 1.5 这类带小数或
    // 指数的 JSON 数字会被只吃掉整数部分，剩下的 ".0" 让整行解析失败，
    // 于是这一行被静默丢弃 —— 驱动等不到该 id 的响应会一直等到超时，整个用例挂掉。
    if (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
        !std::isspace((unsigned char)s_[i_])) {
      i_ = b;
      return false;
    }
    out = std::strtoll(s_.substr(b, i_ - b).c_str(), nullptr, 10);
    return true;
  }

  // 跳过任意 JSON 值
  // depth 上限 64：过深的嵌套既不是本协议需要的，也会让递归/循环占用失控。
  bool skip_value() {
    ws();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '"') { std::string tmp; return str(tmp); }
    if (c == '{' || c == '[') {
      int depth = 0;
      while (i_ < s_.size()) {
        char ch = s_[i_];
        if (ch == '"') { std::string tmp; if (!str(tmp)) return false; continue; }
        if (ch == '{' || ch == '[') {
          if (++depth > 64) return false;   // 嵌套过深：按非法请求处理
        } else if (ch == '}' || ch == ']') {
          --depth;
          ++i_;
          if (depth == 0) return true;
          continue;
        }
        ++i_;
      }
      return false;
    }
    // 数字或 true/false/null：读到分隔符为止
    size_t b = i_;
    while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
           !std::isspace((unsigned char)s_[i_]))
      ++i_;
    return i_ > b;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

 public:
  // 供上层记录某个字段的原始文本（如 id 的非整数拼写）
  size_t pos() const { return i_; }
  std::string raw(size_t from) const {
    size_t b = from < s_.size() ? from : s_.size();
    return s_.substr(b, i_ > b ? i_ - b : 0);
  }
};

}  // namespace

bool parse_json_request(const std::string& line, JsonRequest& out) {
  Parser p(line);
  if (!p.eat('{')) return false;
  if (p.eat('}')) return true;   // 空对象：字段全默认
  std::set<std::string> seen_keys;   // 重复键按非法请求处理（题包要求）
  for (;;) {
    std::string key;
    if (!p.str(key)) return false;
    if (!seen_keys.insert(key).second) return false;
    if (!p.eat(':')) return false;
    if (key == "id") {
      size_t id_begin = p.pos();
      if (p.integer(out.id)) {
        out.has_id = true;
        out.id_raw = p.raw(id_begin);
      } else {
        // id 不是规范整数（1.0 / 1e0 / 1.5 / "1" 等）。
        // 仍然消费掉这个值让整行能解析完，并记下原始文本：
        // 协议要求 id 原样回传，绝不能因此丢弃整条请求（否则驱动等到超时）。
        if (!p.skip_value()) return false;
        out.id_non_integer = true;
        out.id_raw = p.raw(id_begin);
        // 但两类必须判为非法：数组 id，以及溢出成 Infinity 的数字（1e999）。
        // JSON 不允许 Infinity，回传它会让对端解析失败；数组 id 则不是协议允许的形态。
        const std::string& raw = out.id_raw;
        if (!raw.empty() && raw[0] == '[') return false;
        if (!raw.empty() && (raw[0] == '-' || raw[0] == '+' || std::isdigit((unsigned char)raw[0]))) {
          char* endp = nullptr;
          double dv = std::strtod(raw.c_str(), &endp);
          if (!std::isfinite(dv)) return false;   // 1e999 / -1e999 之类
        }
      }
    } else if (key == "op") {
      if (!p.str(out.op)) return false;
    } else if (key == "sql") {
      if (!p.str(out.sql)) return false;
    } else if (key == "search_mode") {
      if (!p.str(out.search_mode)) return false;
    } else {
      if (!p.skip_value()) return false;
    }
    if (p.eat(',')) continue;
    if (p.eat('}')) break;
    return false;
  }
  p.ws();
  return true;   // 允许行尾有空白
}

std::string strip_sql_terminator(const std::string& sql) {
  size_t b = sql.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  size_t e = sql.find_last_not_of(" \t\r\n");
  std::string t = sql.substr(b, e - b + 1);
  while (!t.empty() && (t.back() == ';' || t.back() == ' ' || t.back() == '\t' ||
                        t.back() == '\n' || t.back() == '\r'))
    t.pop_back();
  return t;
}

}  // namespace mvdb
