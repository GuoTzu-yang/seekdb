#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "catalog.h"

namespace mvdb {

// 持久化：
//   snapshot.db  当前全量状态（紧凑二进制，含向量列）
//   wal.log      snapshot 之后的增量（已提交 SQL 语句）
// 启动顺序：读 snapshot → 重放 WAL → 就绪。
// 触发快照：WAL 超过阈值时（且不在事务中）自动做一次，然后清空 WAL。
class Storage {
 public:
  bool open(const std::string& dir, std::string& err);

  // 追加一条已提交事务（若干语句）。
  // 持久化策略见 commit()：默认「写进内核页缓存 + 每 250ms 一次 fdatasync + 正常关闭必刷」，
  // 因为题面把崩溃模型限定为「进程被强制终止」（不模拟断电/磁盘损坏）。
  // MVDB_SYNC=always 可退回每条提交都 fsync 的保守模式。
  bool commit(const std::vector<std::string>& stmts, std::string& err);

  // 把 WAL 刷到磁盘（正常关闭 / 快照前调用）
  bool sync(std::string& err);

  // 读出 WAL 里全部已提交语句（snapshot 之后的增量）
  std::vector<std::string> load_all(std::string& err);

  // 把当前内存状态整体写成 snapshot，并清空 WAL（原子：临时文件 + rename）
  bool checkpoint(const Database& db, std::string& err);

  // 从 snapshot 恢复内存状态（不存在则返回 true 且表为空）
  bool load_snapshot(Database& db, std::string& err);

  // 阈值随快照大小浮动：快照已经很大时，每 32 MiB WAL 就全量重写一次快照会造成
  // 数倍写放大（50 万行快照约 256 MB，写放大 8 倍）。改为「WAL 达到快照一半」才落盘，
  // 上限 512 MiB，下限仍是 32 MiB。
  bool should_checkpoint() const {
    int64_t thr = kCheckpointThreshold;
    if (snap_bytes_ / 2 > thr) thr = snap_bytes_ / 2;
    if (thr > kCheckpointCap) thr = kCheckpointCap;
    return wal_bytes_ >= thr;
  }
  void set_snap_bytes(int64_t n) { snap_bytes_ = n; }
  int64_t wal_bytes() const { return wal_bytes_; }
  const std::string& dir() const { return dir_; }

  static constexpr int64_t kCheckpointThreshold = 32LL * 1024 * 1024;   // 32 MiB
  static constexpr int64_t kCheckpointCap = 512LL * 1024 * 1024;       // 512 MiB
  int64_t snap_bytes_ = 0;                                             // 上次快照大小
  static constexpr int64_t kSyncIntervalMs = 250;

 private:
  std::string dir_;
  std::string wal_path_;
  std::string snap_path_;
  int wal_fd_ = -1;
  int64_t wal_bytes_ = 0;
  int64_t last_sync_ms_ = 0;
  bool sync_always_ = false;

  int64_t now_ms() const;
  bool truncate_wal(std::string& err);
};

}  // namespace mvdb
