#include "vec.h"
#include <cmath>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define MVDB_X86 1
#include <immintrin.h>
#endif

namespace mvdb {

namespace {

// ---------- 标量实现（任何平台都能跑） ----------
void l2_sq_scalar_batch(const float* base, const float* q, int32_t dim,
                        size_t n, double* out) {
  for (size_t r = 0; r < n; ++r) {
    const float* a = base + r * (size_t)dim;
    double acc = 0.0;
    for (int32_t i = 0; i < dim; ++i) {
      double d = (double)a[i] - (double)q[i];
      acc += d * d;
    }
    out[r] = acc;
  }
}

#if MVDB_X86
// ---------- AVX2：double 累加（精度优先）----------
// 历史教训：早先用 __m256 float 双累加器，吞吐高但精度差——
// 128 维 float32 累加的相对误差约 7e-7，而题面容差是 1e-5，
// 大分量向量下绝对误差会越过容差并让 TopK 排错序。
// 现在把 float 差值转成 double 再 FMA：每步都精确（float32 差值在 double 里无损），
// 累加误差降到约 2.5e-15，代价是每指令 4 个 double 而不是 8 个 float。
__attribute__((target("avx2,fma")))
void l2_sq_avx2_batch(const float* base, const float* q, int32_t dim,
                      size_t n, double* out) {
  for (size_t r = 0; r < n; ++r) {
    const float* a = base + r * (size_t)dim;
    __m256d s0 = _mm256_setzero_pd();
    __m256d s1 = _mm256_setzero_pd();
    int32_t i = 0;
    for (; i + 16 <= dim; i += 16) {
      __m256 f0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(q + i));
      __m256 f1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(q + i + 8));
      __m256d d0 = _mm256_cvtps_pd(_mm256_castps256_ps128(f0));
      __m256d d1 = _mm256_cvtps_pd(_mm256_extractf128_ps(f0, 1));
      __m256d d2 = _mm256_cvtps_pd(_mm256_castps256_ps128(f1));
      __m256d d3 = _mm256_cvtps_pd(_mm256_extractf128_ps(f1, 1));
      s0 = _mm256_fmadd_pd(d0, d0, s0);
      s0 = _mm256_fmadd_pd(d1, d1, s0);
      s1 = _mm256_fmadd_pd(d2, d2, s1);
      s1 = _mm256_fmadd_pd(d3, d3, s1);
    }
    for (; i + 8 <= dim; i += 8) {
      __m256 f0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(q + i));
      __m256d d0 = _mm256_cvtps_pd(_mm256_castps256_ps128(f0));
      __m256d d1 = _mm256_cvtps_pd(_mm256_extractf128_ps(f0, 1));
      s0 = _mm256_fmadd_pd(d0, d0, s0);
      s0 = _mm256_fmadd_pd(d1, d1, s0);
    }
    __m256d s = _mm256_add_pd(s0, s1);
    double tmp[4];
    _mm256_storeu_pd(tmp, s);
    double acc = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < dim; ++i) {
      double d = (double)a[i] - (double)q[i];
      acc += d * d;
    }
    out[r] = acc;
  }
}
#endif

bool cpu_has_avx2() {
#if MVDB_X86
  static const bool v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return v;
#else
  return false;
#endif
}

}  // namespace

double l2_distance(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  return l2_distance_ptr(a.data(), b.data(), (int32_t)n);
}

void l2_distance_sq_batch(const float* base, const float* query, int32_t dim,
                          size_t n, double* out) {
  if (n == 0) return;
  if (dim <= 0 || base == nullptr || query == nullptr) {
    for (size_t r = 0; r < n; ++r) out[r] = 0.0;
    return;
  }
#if MVDB_X86
  if (cpu_has_avx2()) {
    l2_sq_avx2_batch(base, query, dim, n, out);
    return;
  }
#endif
  l2_sq_scalar_batch(base, query, dim, n, out);
}

double l2_distance_sq_ptr(const float* a, const float* b, int32_t dim) {
  if (dim <= 0) return 0.0;
  double out = 0.0;
  l2_distance_sq_batch(a, b, dim, 1, &out);
  return out;
}

double l2_distance_ptr(const float* a, const float* b, int32_t dim) {
  return std::sqrt(l2_distance_sq_ptr(a, b, dim));
}

// 快速十进制浮点解析（热路径，见 vec.h 说明）
const char* parse_float_fast(const char* p, const char* end, float& out) {
  static const double kPow10[] = {
      1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  if (p >= end) return nullptr;
  bool neg = false;
  if (*p == '-' || *p == '+') { neg = (*p == '-'); ++p; }
  double mant = 0.0;
  int exp10 = 0;
  bool any = false;
  while (p < end && *p >= '0' && *p <= '9') {
    mant = mant * 10.0 + (double)(*p - '0');
    ++p;
    any = true;
  }
  if (p < end && *p == '.') {
    ++p;
    while (p < end && *p >= '0' && *p <= '9') {
      mant = mant * 10.0 + (double)(*p - '0');
      --exp10;
      ++p;
      any = true;
    }
  }
  if (!any) return nullptr;
  if (p < end && (*p == 'e' || *p == 'E')) {
    ++p;
    bool eneg = false;
    if (p < end && (*p == '-' || *p == '+')) { eneg = (*p == '-'); ++p; }
    int e = 0;
    bool eany = false;
    while (p < end && *p >= '0' && *p <= '9') {
      if (e < 100000) e = e * 10 + (*p - '0');
      ++p;
      eany = true;
    }
    if (!eany) return nullptr;
    exp10 += eneg ? -e : e;
  }
  double v = mant;
  if (exp10 != 0) {
    // 尾数为 0 时结果恒为 0（含 0e9999 这种写法）。
    // 不能走 pow()：0 * inf 会得到 NaN，把一个合法的 0 判成非法向量。
    if (mant == 0.0) {
      out = 0.0f;
      return p;
    }
    if (exp10 > 0 && exp10 < (int)(sizeof(kPow10) / sizeof(kPow10[0])))
      v = mant * kPow10[exp10];
    else if (exp10 < 0 && -exp10 < (int)(sizeof(kPow10) / sizeof(kPow10[0])))
      v = mant / kPow10[-exp10];
    else
      v = mant * std::pow(10.0, (double)exp10);   // 极值交回库函数（会得 inf/0）
  }
  out = (float)(neg ? -v : v);
  return p;
}

bool to_f32(double d, float& out) {  if (!std::isfinite(d)) return false;
  if (std::fabs(d) > 3.402823466e+38) return false;
  float f = (float)d;
  if (!std::isfinite(f)) return false;
  out = f;
  return true;
}

bool validate_vector(const std::vector<float>& v, int32_t dim, std::string& err) {
  if (dim > 0 && (int32_t)v.size() != dim) {
    err = "vector dimension mismatch";
    return false;
  }
  for (float f : v) {
    if (!std::isfinite(f)) { err = "vector value not finite"; return false; }
  }
  return true;
}

}  // namespace mvdb
