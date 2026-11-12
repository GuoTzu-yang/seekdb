#pragma once
#include <string>

namespace mvdb {

// 题目 JSONL 协议的一行请求：
//   {"id":1,"op":"execute","sql":"SELECT ...;","search_mode":"exact"}
//   {"id":4,"op":"shutdown"}
// 只需要取出这四个字段；未知字段整体跳过。
struct JsonRequest {
  long long id = 0;
  bool has_id = false;
  // id 的原始 JSON 文本（如 "1"、"1.0"、"1e0"）。协议要求 id 原样回传，
  // 所以非整数形态也要能回出去，而不是无声丢弃整条请求。
  std::string id_raw;
  bool id_non_integer = false;   // 出现了 id 但不是规范整数
  std::string op;
  std::string sql;
  std::string search_mode = "exact";
};

// 解析一行 JSON。失败返回 false（调用方按协议错误处理）。
bool parse_json_request(const std::string& line, JsonRequest& out);

// 去掉 SQL 尾部的空白与分号（协议按整串下发，不能再按 ';' 切分）
std::string strip_sql_terminator(const std::string& sql);

}  // namespace mvdb
