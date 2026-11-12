#pragma once
#include <string>
#include <vector>
#include <cctype>
#include "common.h"

namespace mvdb {

enum class Tok : uint8_t {
  END, IDENT, INT_LIT, STR, PUNCT
};

struct Token {
  Tok kind = Tok::END;
  std::string text;   // IDENT: 原文；STR: 内容；PUNCT: 符号
  int64_t ival = 0;   // INT_LIT（在 uint64 范围内时按位解释）
  // 整数字面量的无符号值：用于正确处理 BIGINT 边界
  // （-9223372036854775808 的绝对值超出 int64，需要 2^63 这个无符号值）
  unsigned long long uval = 0;
  bool big = false;   // 绝对值 > INT64_MAX
  size_t pos = 0;
};

// 把一段 SQL 切成 token。字符串用单引号，支持 '' 转义。
std::vector<Token> lex(const std::string& sql);

// 大小写不敏感比较
bool ieq(const std::string& a, const char* b);

}  // namespace mvdb
