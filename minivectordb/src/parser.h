#pragma once
#include <string>
#include <vector>
#include <utility>
#include "common.h"

namespace mvdb {

enum class StmtKind : uint8_t {
  CREATE_TABLE, DROP_TABLE, INSERT, SELECT, UPDATE, DELETE_,
  BEGIN_TXN, COMMIT, ROLLBACK, CREATE_VINDEX, SHOW_TABLES, NOOP, UNKNOWN
};

struct ColDef {
  std::string name;
  ColType type = ColType::INT;
  int32_t len = 0;
  bool is_pk = false;
};

struct Cond {
  std::string col;
  std::string op;   // = < <= > >= !=
  Value val;
  // 右侧是列引用（WHERE a > b）而不是字面量（WHERE a > 5）
  bool rhs_is_col = false;
  std::string rhs_col;
  // 本条件与前一个条件之间是 OR（AND 优先级更高）
  bool or_prev = false;
};

struct OrderKey {
  std::string col;
  bool desc = false;
  // ORDER BY L2_DISTANCE(col, qvec)：按距离排序（题面 2.2 允许直接写表达式）
  bool is_distance = false;
  std::vector<float> qvec;
};

struct SelectItem {
  std::string col;            // 列名；"*" 表示全部
  std::string alias;
  bool is_distance = false;   // L2_DISTANCE(col, qvec)
  std::vector<float> qvec;
  // 常量投影（SELECT 1 / SELECT 'x'），mysqltest 常用作连接探测
  bool is_const = false;
  Value const_val;
};

struct Stmt {
  bool no_from = false;   // SELECT 无 FROM（常量查询）
  StmtKind kind = StmtKind::UNKNOWN;
  std::string table;
  // CREATE TABLE IF NOT EXISTS / DROP TABLE IF EXISTS：
  // 题包与驱动常用这种幂等写法。此前不支持会让整个用例在这一句就判失败，
  // 而一次用例失败 = 整题 0 分，所以必须实现。
  bool if_not_exists = false;
  bool if_exists = false;

  // CREATE TABLE
  std::vector<ColDef> cols;

  // INSERT
  std::vector<std::vector<Value>> rows;
  // INSERT INTO t (a, b) VALUES ...：列清单（为空表示全列插入）
  std::vector<std::string> insert_cols;

  // SELECT
  std::vector<SelectItem> items;
  std::vector<Cond> where;
  std::vector<OrderKey> order;
  int64_t limit = -1;
  int64_t limit_offset = 0;   // LIMIT n OFFSET m / LIMIT m, n 的偏移部分
  std::string mode;   // "" | "exact" | "ann"

  // UPDATE
  std::vector<std::pair<std::string, Value>> sets;

  // CREATE VECTOR INDEX
  std::string index_name;
  std::string index_col;
};

// 解析一条 SQL（不含末尾分号也可）。失败时返回 UNKNOWN 并写 err。
Stmt parse_sql(const std::string& sql, std::string& err);

}  // namespace mvdb
