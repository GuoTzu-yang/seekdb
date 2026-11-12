#pragma once
#include <cstdio>
#include "exec.h"

namespace mvdb {

// stdout 协议层。所有对外输出格式集中在这里。
//
// 题面 3.1 / 3.2：驱动通过 stdin/stdout 发送 SQL 并「统一比较输出」，结果集格式
// 遵循配套驱动；题包 .result 是 JSON 数组，每个元素与该条 SQL 一一对应
// （`{}` / `affected_rows` / `columns`+`rows` / `error`）。
// 因此默认格式取 JSON：一条语句输出一行裸 JSON 对象，无信封。
// 语句边界由换行确定，不需要额外的行数摘要行。
enum class WireFormat {
  // 默认：每语句一行 JSON
  //   查询   -> {"columns":[...],"rows":[[...]]}
  //   DML    -> {"affected_rows":N}
  //   DDL/事务控制 -> {}
  //   错误   -> {"error":"<语义名>"}
  JSON = 0,
  // 自定文本格式（调试用，MVDB_WIRE=text）
  //   查询 -> 表头行 + 数据行（'\t' 分隔）+ "-- N row(s)"
  //   DML  -> "OK <n>"
  //   错误 -> "ERROR <code>: <msg>"
  SELF_DEFINED = 1,
  // 精简文本格式（MVDB_WIRE=bare）：不输出 "-- N row(s)" 这类附加行
  BARE = 2,
};

WireFormat wire_format_from_env();
const char* wire_format_name(WireFormat f);

// 错误码 -> 题包语义名（duplicate_key / unsupported_sql / dimension_mismatch ...）
const char* err_semantic_name(int code);

// 把一条执行结果写出去（含 flush）。一条语句只写一次，绝不写多行。
void write_result(const Result& r, WireFormat fmt, FILE* out);

// ---- 题目规定的 JSONL 协议（题包 STDIO_PROTOCOL.md）----
// 每条请求恰好回一条响应，必须带原样回传的 id（数字不能转字符串）与布尔 ok。
// 成功且无额外字段（建表、事务控制等）时只输出 {"id":N,"ok":true}。
// id_raw 非空时优先用它输出 id：用于请求里 id 写成 1.0 / 1e0 等非整数形态时
// 仍然原样回传，而不是丢弃整条请求（丢弃会让驱动等到超时）。
void write_jsonl_response(const Result& r, long long id, FILE* out,
                          const std::string& id_raw = std::string());

// 启动握手：必须精确输出 {"type":"ready"}，不能多字段、不能是 READY 文本。
void write_jsonl_ready(FILE* out);

}  // namespace mvdb
