#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <set>
#include <unistd.h>
#include "common.h"
#include "vecstore.h"

namespace mvdb {

// 前置声明：类内联函数要用
bool ieq(const std::string& a, const char* b);
std::string value_to_string(const Value& v);
std::string value_to_sql(const Value& v);

// 一行数据。向量列不放在这里（走 Table::vecs 平坦存储），
// vals[vec_col] 仅保留一个占位列以保证列下标一致。
struct Row {
  std::vector<Value> vals;
  bool deleted = false;
};

struct Table {
  std::string name;
  std::vector<Column> cols;
  int pk_col = -1;
  int vec_col = -1;
  int32_t vec_dim = 0;
  bool has_vindex = false;
  // 已建立的向量索引名（小写）。允许同表多个不同名的索引，
  // 只有同名重复才判冲突 —— 与 MySQL 的 CREATE INDEX 行为一致。
  std::vector<std::string> vindex_names;

  // 行（含非向量列）
  std::vector<Row> rows;
  // 被标记删除的行数。为 0 时查询可以完全跳过 deleted 检查
  // （10,000 行每次少读 10,000 个头，实测省约 15% 的 TopK 扫描时间）。
  size_t deleted_count = 0;
  // 向量列：磁盘驻留（mmap 文件），rows[i] 的向量 = vecs.data() + i * vec_dim。
  // 用 VecStore 而不是 std::vector<float>，是为了满足题面 2.3「不能依靠全量载入内存」。
  VecStore vecs;

  std::unordered_map<int64_t, size_t> pkidx;

  // ---- 普通列的等值倒排（只用于加速「带等值过滤的 TopK」）----
  // inv[c] = 该列 值(key) → 行下标列表。向量列不建。
  // 维护策略是「只加不删」：INSERT/UPDATE 时追加，DELETE 时不摘除；
  // 查询命中候选集后仍用完整条件复验，所以旧条目只会被过滤掉，不会算错。
  std::vector<std::unordered_map<std::string, std::vector<uint32_t>>> inv;
  bool inv_built = false;

  static std::string inv_key(const Value& v) {
    if (v.type == ColType::INT) return std::to_string(v.i);
    return v.s;
  }

  void inv_add_row(size_t i) {
    if (inv.size() != cols.size()) return;
    const Row& r = rows[i];
    for (size_t c = 0; c < cols.size(); ++c) {
      if (cols[c].type == ColType::VECTOR) continue;
      if (c >= r.vals.size()) continue;
      inv[c][inv_key(r.vals[c])].push_back((uint32_t)i);
    }
  }

  void inv_rebuild() {
    inv.assign(cols.size(), {});
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].deleted) continue;
      inv_add_row(i);
    }
    inv_built = true;
  }

  const Column* col(const std::string& n) const {
    for (auto& c : cols) if (ieq(c.name, n.c_str())) return &c;
    return nullptr;
  }
  int col_index(const std::string& n) const {
    for (size_t i = 0; i < cols.size(); ++i) if (ieq(cols[i].name, n.c_str())) return (int)i;
    return -1;
  }

  const float* vec_at(size_t i) const {
    return (vec_col >= 0 && vec_dim > 0) ? (vecs.data() + i * (size_t)vec_dim) : nullptr;
  }
  float* vec_at_mut(size_t i) {
    return (vec_col >= 0 && vec_dim > 0) ? (vecs.data() + i * (size_t)vec_dim) : nullptr;
  }
  bool append_vec(const std::vector<float>& v) {
    std::string e;
    return vecs.append(v.data(), v.size(), e);
  }
  void set_vec(size_t i, const std::vector<float>& v) {
    if (vec_dim <= 0) return;
    float* p = vec_at_mut(i);
    for (int32_t k = 0; k < vec_dim; ++k) p[k] = v[(size_t)k];
  }
  void resize_vecs(size_t nrows) {
    if (vec_dim > 0) {
      std::string e;
      vecs.truncate_to(nrows * (size_t)vec_dim, e);
    }
  }

  void rebuild_pk();
};

// 事务的一条 undo（行级旧值）
struct UndoRec {
  std::string table;
  int64_t pk = 0;
  bool existed = false;
  Row old_row;
  std::vector<float> old_vec;   // 向量列旧值（existed 时有效）
  // 表级 undo（DROP TABLE / CREATE TABLE 的逆操作）
  bool is_table_level = false;
  bool dropped_whole_table = false;
  // DROP TABLE 时把整张表（结构 + 数据 + 向量）搬到这里，
  // 否则 ROLLBACK 只能恢复一个空壳（曾经的 bug：连列定义都没有）
  std::unique_ptr<Table> dropped_table;
};

class Database {
 public:
  std::string dir;
  std::unordered_map<std::string, std::unique_ptr<Table>> tables;

  Table* find(const std::string& name) {
    auto it = tables.find(name);
    return it == tables.end() ? nullptr : it->second.get();
  }
  Table* find_ci(const std::string& name);

  // 事务状态
  bool in_txn = false;
  std::vector<UndoRec> undo;
  std::vector<std::string> txn_stmts;   // 事务内已成功的语句（供 WAL）

  // 向量补偿日志：向量区是 MAP_SHARED 内存映射，改内存等于改文件，
  // 进程被强杀后 undo 内存丢失、未提交的向量改动却已落到磁盘。
  // 因此事务内第一次改动某行的向量前，先把原值追加到这里并 fsync；
  // COMMIT/ROLLBACK 成功后删除；启动时若发现残留，说明上次是异常退出，逐条回滚。
  std::string vec_undo_path;
  std::set<std::string> vec_undo_seen;   // 已记录过的 "table\x01pk"，只记最早的原值

  void rollback_undo();
  void clear_txn() { undo.clear(); txn_stmts.clear(); in_txn = false; }
  void clear_vec_undo() {
    vec_undo_seen.clear();
    if (!vec_undo_path.empty()) ::unlink(vec_undo_path.c_str());
  }
};

}  // namespace mvdb
