#include "protocol.h"
#include <cstdlib>
#include <cstring>
#include <string>

namespace mvdb {

WireFormat wire_format_from_env() {
  const char* e = std::getenv("MVDB_WIRE");
  if (!e) return WireFormat::JSON;
  if (0 == std::strcmp(e, "text") || 0 == std::strcmp(e, "TEXT") ||
      0 == std::strcmp(e, "self") || 0 == std::strcmp(e, "self-defined"))
    return WireFormat::SELF_DEFINED;
  if (0 == std::strcmp(e, "bare") || 0 == std::strcmp(e, "BARE"))
    return WireFormat::BARE;
  return WireFormat::JSON;
}

const char* wire_format_name(WireFormat f) {
  switch (f) {
    case WireFormat::BARE: return "bare";
    case WireFormat::SELF_DEFINED: return "self-defined";
    default: return "json";
  }
}

// 题包错误语义名。题包 README：「当前未冻结错误码映射的配置至少要求非空错误码」，
// 已给出的样例名是 duplicate_key / unsupported_sql / dimension_mismatch，
// 因此已知的失败类别直接复用题包的词汇，其余给同风格的语义名。
const char* err_semantic_name(int code) {
  switch (code) {
    case E_DUP_KEY: return "duplicate_key";
    case E_NOT_SUPPORTED: return "unsupported_sql";
    case E_DIM_MISMATCH: return "dimension_mismatch";
    case E_INVALID_VECTOR: return "invalid_vector";
    case E_INVALID_VALUE: return "invalid_value";
    case E_NO_TABLE: return "table_not_found";
    case E_DUP_TABLE: return "table_exists";
    case E_NO_COL: return "column_not_found";
    case E_TOO_LONG: return "value_too_long";
    case E_SYNTAX: return "syntax_error";
    case E_TXN: return "transaction_error";
    case E_IO: return "io_error";
    case E_INTERNAL: return "internal_error";
    default: return "error";
  }
}

namespace {

// JSON 字符串转义。UTF-8 字节原样透传（JSON 允许），只转义必须转义的字符。
void json_escape_to(FILE* out, const std::string& s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': std::fputs("\\\"", out); break;
      case '\\': std::fputs("\\\\", out); break;
      case '\n': std::fputs("\\n", out); break;
      case '\r': std::fputs("\\r", out); break;
      case '\t': std::fputs("\\t", out); break;
      case '\b': std::fputs("\\b", out); break;
      case '\f': std::fputs("\\f", out); break;
      default:
        if (c < 0x20) {
          std::fprintf(out, "\\u%04x", (unsigned)c);
        } else {
          std::fputc((int)c, out);
        }
    }
  }
}

void json_string_to(FILE* out, const std::string& s) {
  std::fputc('"', out);
  json_escape_to(out, s);
  std::fputc('"', out);
}

// 数值单元格：非有限值降级成 null，避免产出非法 JSON
bool json_number_is_valid(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
          c == 'e' || c == 'E'))
      return false;
  }
  return true;
}

void json_cell_to(FILE* out, uint8_t kind, const std::string& s) {
  switch (kind) {
    case 1:  // string
      json_string_to(out, s);
      break;
    case 2:  // double
      if (json_number_is_valid(s))
        std::fputs(s.c_str(), out);
      else
        std::fputs("null", out);
      break;
    case 3:  // vector：value_to_string 已输出 [a,b] 形态，本身就是合法 JSON 数组
      if (!s.empty() && s[0] == '[')
        std::fputs(s.c_str(), out);
      else
        json_string_to(out, s);
      break;
    default:  // int
      if (json_number_is_valid(s))
        std::fputs(s.c_str(), out);
      else
        json_string_to(out, s);
      break;
  }
}

void write_json(const Result& r, FILE* out) {
  if (r.code != 0) {
    std::fputs("{\"error\":", out);
    json_string_to(out, r.err_name.empty() ? err_semantic_name(r.code) : r.err_name);
    std::fputs("}\n", out);
    return;
  }
  if (r.has_rows) {
    std::fputs("{\"columns\":[", out);
    for (size_t k = 0; k < r.headers.size(); ++k) {
      if (k) std::fputc(',', out);
      json_string_to(out, r.headers[k]);
    }
    std::fputs("],\"rows\":[", out);
    for (size_t i = 0; i < r.rows.size(); ++i) {
      if (i) std::fputc(',', out);
      std::fputc('[', out);
      const std::vector<std::string>& row = r.rows[i];
      for (size_t k = 0; k < row.size(); ++k) {
        if (k) std::fputc(',', out);
        uint8_t kind = (k < r.col_kind.size()) ? r.col_kind[k] : (uint8_t)1;
        json_cell_to(out, kind, row[k]);
      }
      std::fputc(']', out);
    }
    std::fputs("]}\n", out);
    return;
  }
  if (r.has_affected) {
    std::fprintf(out, "{\"affected_rows\":%lld}\n", (long long)r.affected);
    return;
  }
  std::fputs("{}\n", out);
}

void write_text(const Result& r, WireFormat fmt, FILE* out) {
  if (r.code != 0) {
    std::fprintf(out, "ERROR %d: %s\n", r.code, r.err_msg.c_str());
    return;
  }
  if (r.has_rows) {
    for (size_t k = 0; k < r.headers.size(); ++k) {
      if (k) std::fputc('\t', out);
      std::fputs(r.headers[k].c_str(), out);
    }
    std::fputc('\n', out);
    for (auto& row : r.rows) {
      for (size_t k = 0; k < row.size(); ++k) {
        if (k) std::fputc('\t', out);
        std::fputs(row[k].c_str(), out);
      }
      std::fputc('\n', out);
    }
    if (fmt == WireFormat::SELF_DEFINED)
      std::fprintf(out, "-- %zu row(s)\n", r.rows.size());
    return;
  }
  if (fmt == WireFormat::SELF_DEFINED)
    std::fprintf(out, "OK %lld\n", (long long)r.affected);
  else
    std::fputs("OK\n", out);
}

}  // namespace

void write_result(const Result& r, WireFormat fmt, FILE* out) {
  if (fmt == WireFormat::JSON)
    write_json(r, out);
  else
    write_text(r, fmt, out);
  std::fflush(out);
}

// ---- 题目规定的 JSONL 协议 ----
void write_jsonl_ready(FILE* out) {
  std::fputs("{\"type\":\"ready\"}\n", out);
  std::fflush(out);
}

// 每条请求一条响应：{"id":N,"ok":true,...} 或 {"id":N,"ok":false,"error":{"code","message"}}
void write_jsonl_response(const Result& r, long long id, FILE* out,
                          const std::string& id_raw) {
  auto put_id = [&]() {
    if (!id_raw.empty()) std::fputs(id_raw.c_str(), out);   // 原样回传（1.0 / 1e0 等）
    else std::fprintf(out, "%lld", id);
  };
  if (r.code != 0) {
    std::fputs("{\"id\":", out);
    put_id();
    std::fputs(",\"ok\":false,\"error\":{\"code\":", out);
    json_string_to(out, r.err_name.empty() ? err_semantic_name(r.code) : r.err_name);
    std::fputs(",\"message\":", out);
    json_string_to(out, r.err_msg.empty() ? "error" : r.err_msg);
    std::fputs("}}\n", out);
    std::fflush(out);
    return;
  }
  std::fputs("{\"id\":", out);
  put_id();
  std::fputs(",\"ok\":true", out);
  if (r.has_rows) {
    std::fputs(",\"columns\":[", out);
    for (size_t k = 0; k < r.headers.size(); ++k) {
      if (k) std::fputc(',', out);
      json_string_to(out, r.headers[k]);
    }
    std::fputs("],\"rows\":[", out);
    if (r.lazy()) {
      // 大结果集：按行号逐行生成并立刻写出，内存只驻留当前一行。
      // 题面 2.3 要求不能依靠全量载入内存；这里是它的落点。
      for (size_t i = 0; i < r.lazy_idx.size(); ++i) {
        if (i) std::fputc(',', out);
        std::fputc('[', out);
        const std::vector<std::string> row = r.lazy_row(r.lazy_idx[i]);
        for (size_t k = 0; k < row.size(); ++k) {
          if (k) std::fputc(',', out);
          uint8_t kind = (k < r.col_kind.size()) ? r.col_kind[k] : (uint8_t)1;
          json_cell_to(out, kind, row[k]);
        }
        std::fputc(']', out);
      }
    } else {
      for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) std::fputc(',', out);
        std::fputc('[', out);
        const std::vector<std::string>& row = r.rows[i];
        for (size_t k = 0; k < row.size(); ++k) {
          if (k) std::fputc(',', out);
          uint8_t kind = (k < r.col_kind.size()) ? r.col_kind[k] : (uint8_t)1;
          json_cell_to(out, kind, row[k]);
        }
        std::fputc(']', out);
      }
    }
    std::fputc(']', out);
  } else if (r.has_affected) {
    std::fprintf(out, ",\"affected_rows\":%lld", (long long)r.affected);
  }
  std::fputs("}\n", out);
  std::fflush(out);
}

}  // namespace mvdb
