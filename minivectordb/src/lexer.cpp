#include "lexer.h"
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace mvdb {

bool ieq(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  }
  return true;
}

static bool is_punct(char c) {
  switch (c) {
    case ',': case '(': case ')': case '[': case ']':
    case '=': case '<': case '>': case ';': case '*':
    case '.': case '-': case '+': case '/':
      return true;
    default: return false;
  }
}

std::vector<Token> lex(const std::string& sql) {
  std::vector<Token> out;
  size_t i = 0, n = sql.size();
  while (i < n) {
    char c = sql[i];
    if (std::isspace((unsigned char)c)) { ++i; continue; }

    // 注释：-- 行注释、# 行注释、/* 块注释 */（WAL 重放的语句可能带）
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      i = (i + 1 < n) ? i + 2 : n;
      continue;
    }

    // 字符串字面量
    if (c == '\'' || c == '"') {
      char q = c; ++i;
      std::string s;
      while (i < n) {
        if (sql[i] == q) {
          if (i + 1 < n && sql[i + 1] == q) { s.push_back(q); i += 2; continue; }
          ++i; break;
        }
        s.push_back(sql[i++]);
      }
      Token t; t.kind = Tok::STR; t.text = std::move(s); t.pos = i;
      out.push_back(std::move(t));
      continue;
    }

    // 数字：整数 → INT_LIT；带小数点/科学计数法 → IDENT（保留原文，给向量解析用）
    // 另外把「'.' 紧跟数字」也算数字开头：-.5e+1 / .5 是合法 SQL 写法，
    // 否则 '.' 会先被切成 PUNCT，向量解析随即报 "expected number in vector"。
    if (std::isdigit((unsigned char)c) ||
        (c == '.' && i + 1 < n && std::isdigit((unsigned char)sql[i + 1]))) {
      size_t st = i;
      while (i < n) {
        char d = sql[i];
        if (std::isdigit((unsigned char)d) || d == '.') { ++i; continue; }
        if ((d == 'e' || d == 'E') && i + 1 < n) { ++i; continue; }
        if ((d == '-' || d == '+') && i > st && (sql[i-1] == 'e' || sql[i-1] == 'E')) { ++i; continue; }
        break;
      }
      std::string raw = sql.substr(st, i - st);
      Token t; t.pos = st; t.text = raw;
      if (raw.find_first_of(".eE") == std::string::npos) {
        t.kind = Tok::INT_LIT;
        // 用无符号解析：BIGINT 下界 -9223372036854775808 的绝对值恰为 2^63，
        // 超出 int64 上界，必须用 unsigned long long 承载才能正确取负。
        errno = 0;
        char* endp = nullptr;
        unsigned long long u = std::strtoull(raw.c_str(), &endp, 10);
        t.uval = u;
        t.big = (errno == ERANGE) || (u > (unsigned long long)INT64_MAX);
        t.ival = (int64_t)u;
      } else {
        t.kind = Tok::IDENT;   // 浮点原文，例如 1.5 / 1e-3
      }
      out.push_back(std::move(t));
      continue;
    }

    // 标识符 / 关键字
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t st = i;
      while (i < n && (std::isalnum((unsigned char)sql[i]) || sql[i] == '_')) ++i;
      Token t; t.kind = Tok::IDENT; t.text = sql.substr(st, i - st); t.pos = st;
      out.push_back(std::move(t));
      continue;
    }

    // 标点（先处理双字符）
    if (i + 1 < n) {
      char c2 = sql[i + 1];
      if ((c == '<' && (c2 == '=' || c2 == '>')) || (c == '>' && c2 == '=') || (c == '!' && c2 == '=')) {
        Token t; t.kind = Tok::PUNCT; t.text = std::string(1, c) + c2; t.pos = i;
        out.push_back(std::move(t)); i += 2; continue;
      }
    }
    if (is_punct(c)) {
      Token t; t.kind = Tok::PUNCT; t.text = std::string(1, c); t.pos = i;
      out.push_back(std::move(t)); ++i; continue;
    }

    // 未知字符：跳过（容忍）
    ++i;
  }
  Token e; e.kind = Tok::END;
  out.push_back(e);
  return out;
}

}  // namespace mvdb
