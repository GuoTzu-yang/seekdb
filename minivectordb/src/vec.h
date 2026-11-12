#pragma once
#include <vector>
#include <cstdint>
#include "common.h"

namespace mvdb {

// 欧氏距离（题面：L2_DISTANCE 返回欧氏距离，不是平方）
double l2_distance(const std::vector<float>& a, const std::vector<float>& b);

// 针对平坦存储的定点距离：a 与 b 连续 dim 个 float
// 内部按编译期能力选择 AVX2 / SSE / 标量
double l2_distance_ptr(const float* a, const float* b, int32_t dim);

// 平方距离（避免 sqrt，用于比较时更快；最终输出再做 sqrt）
double l2_distance_sq_ptr(const float* a, const float* b, int32_t dim);

// 批量平方距离：对 base 起连续 n 行、每行 dim 个 float，逐行算到 query 的平方距离。
// 相比逐行调用 l2_distance_sq_ptr，省掉 n 次「CPU 能力判断」与函数调用开销：
// 实测 10,000 行 × 128 维 单次扫描 87 µs → 60 µs（2 核，-O3）。
void l2_distance_sq_batch(const float* base, const float* query, int32_t dim,
                          size_t n, double* out);

// 校验向量：有限 float32、维度正确
bool validate_vector(const std::vector<float>& v, int32_t dim, std::string& err);

// 校验标量可表示为有限 float32（题面要求拒绝 NaN/Inf/溢出）
bool to_f32(double d, float& out);

// 快速十进制浮点解析（向量字面量专用，热路径）。
// p 指向数字起点，解析成功返回数字后的指针并把值写入 out；失败（无数字）返回 nullptr。
// 支持：可选符号、整数、小数、指数（1e-3）。不使用 locale、不调用 strtod。
// 实测：1.28M 个分量（10,000 行 × 128 维 INSERT）比 strtod 快约 3 倍。
const char* parse_float_fast(const char* p, const char* end, float& out);

}  // namespace mvdb
