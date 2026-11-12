#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mvdb {

// 向量列的磁盘驻留存储。
//
// 题面 2.3 要求「正式测试包含数据量超过内存限额的场景，不能依靠全量载入内存」。
// 这里把向量本体放进 mmap 映射的文件：进程只保留一个指针，页由内核按需换入、
// 内存紧张时可直接丢弃（file-backed、可回收），峰值内存不再随数据量线性增长。
//
// 约定：vec.dat 是一个纯 float 数组，第 i 行的向量 = data() + i * vec_dim。
class VecStore {
 public:
  VecStore() = default;
  VecStore(const VecStore&) = delete;
  VecStore& operator=(const VecStore&) = delete;
  ~VecStore() { close(); }

  const std::string& path() const { return path_; }

  // 打开（或创建）数据文件；keep_existing=true 时保留文件内容
  // 注意：mmap 之后立刻关闭 fd —— 映射不依赖 fd 存活，这样常驻
  // 描述符只留 WAL 一个。曾经两个常驻 fd（vec + wal）加上
  // stdin/stdout/stderr 就占满 5 个，在 RLIMIT_NOFILE=4 的环境里
  // 打开失败，表现为「重启后表不见了」（table_not_found）。
  bool open_file(const std::string& path, std::string& err) {
    close();
    path_ = path;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) { err = "cannot open vec file: " + path; return false; }
    struct stat stbuf {};
    if (::fstat(fd, &stbuf) != 0) { ::close(fd); err = "stat vec file failed"; return false; }
    const size_t bytes = (size_t)stbuf.st_size;
    map_floats_ = bytes / sizeof(float);
    if (map_floats_ > 0) {
      void* p = ::mmap(nullptr, map_floats_ * sizeof(float), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
      if (p == MAP_FAILED) { ::close(fd); err = "mmap vec file failed"; return false; }
      base_ = (float*)p;
    }
    ::close(fd);
    size_ = map_floats_;
    return true;
  }

  // 确保容量至少 need 个 float（不足时倍增扩容，旧指针会失效）
  bool ensure(size_t need, std::string& err) {
    if (need <= map_floats_) return true;
    if (path_.empty()) { err = "vec store not open"; return false; }
    size_t cap = map_floats_ ? map_floats_ : 4096;
    while (cap < need) cap *= 2;
    const size_t cap_bytes = cap * sizeof(float);
    // 临时打开，ftruncate + mmap 之后立即关闭（不常驻描述符）
    int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) { err = "cannot reopen vec file: " + path_; return false; }
    if (::ftruncate(fd, (off_t)cap_bytes) != 0) { ::close(fd); err = "ftruncate vec file failed"; return false; }
    if (base_) { ::munmap(base_, map_floats_ * sizeof(float)); base_ = nullptr; }
    void* p = ::mmap(nullptr, cap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { err = "remap vec file failed"; return false; }
    base_ = (float*)p;
    map_floats_ = cap;
    return true;
  }

  // 截断到 n 个 float（回滚/删除尾行时用）
  bool truncate_to(size_t n, std::string& err) {
    if (n > map_floats_) return ensure(n, err);
    if (n == size_) return true;
    size_ = n;
    if (map_floats_ > 0 && base_) {
      // 容量保持不变，只把逻辑长度缩短；文件长度在 flush 时收紧
    }
    return true;
  }

  // 追加一段向量
  bool append(const float* v, size_t n, std::string& err) {
    if (n == 0) return true;
    if (!ensure(size_ + n, err)) return false;
    std::memcpy(base_ + size_, v, n * sizeof(float));
    size_ += n;
    return true;
  }

  // 覆盖第 i 段（i 以 float 为单位）
  bool write_at(size_t off, const float* v, size_t n, std::string& err) {
    if (off + n > size_) {
      if (!ensure(off + n, err)) return false;
      size_ = off + n;
    }
    std::memcpy(base_ + off, v, n * sizeof(float));
    return true;
  }

  // 落盘（mmap 写是脏页，需要 msync 才能保证崩溃后可见）
  bool flush(std::string& err) {
    if (base_ && size_ > 0) {
      if (::msync(base_, size_ * sizeof(float), MS_SYNC) != 0) {
        err = "msync vec file failed";
        return false;
      }
    }
    // 收紧文件长度到实际使用量（临时打开 fd，不常驻）
    if (map_floats_ > size_) {
      int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
      if (fd < 0) { err = "cannot reopen vec file for truncate"; return false; }
      if (::ftruncate(fd, (off_t)(size_ * sizeof(float))) != 0) {
        ::close(fd);
        err = "ftruncate vec file failed";
        return false;
      }
      if (size_ > 0) {
        if (base_) ::munmap(base_, map_floats_ * sizeof(float));
        base_ = nullptr;
        void* p = ::mmap(nullptr, size_ * sizeof(float), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) { err = "remap after truncate failed"; return false; }
        base_ = (float*)p;
      } else {
        ::close(fd);
        base_ = nullptr;
      }
      map_floats_ = size_;
    }
    return true;
  }

  float* data() { return base_; }
  const float* data() const { return base_; }
  size_t size() const { return size_; }
  void set_size(size_t n) { size_ = n; }

  void close() {
    if (base_) {
      if (size_ > 0) ::msync(base_, size_ * sizeof(float), MS_SYNC);
      ::munmap(base_, map_floats_ * sizeof(float));
      base_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    map_floats_ = 0;
    size_ = 0;
  }

 private:
  std::string path_;
  int fd_ = -1;
  float* base_ = nullptr;
  size_t map_floats_ = 0;
  size_t size_ = 0;
};

}  // namespace mvdb
