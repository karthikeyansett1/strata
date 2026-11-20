#include "distance.h"

#include <algorithm>
#include <cmath>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace strata::vector {
namespace {

template <bool kDot>
float Scalar(std::span<const float> a, std::span<const float> b) {
  float total = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if constexpr (kDot) {
      total += a[i] * b[i];
    } else {
      const float delta = a[i] - b[i];
      total += delta * delta;
    }
  }
  return total;
}

#if defined(__x86_64__)
template <bool kDot>
__attribute__((target("avx2"))) float Simd(std::span<const float> a, std::span<const float> b) {
  auto total = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 8 <= a.size(); i += 8) {
    const auto x = _mm256_loadu_ps(a.data() + i);
    const auto y = _mm256_loadu_ps(b.data() + i);
    if constexpr (kDot) {
      total = _mm256_add_ps(total, _mm256_mul_ps(x, y));
    } else {
      const auto delta = _mm256_sub_ps(x, y);
      total = _mm256_add_ps(total, _mm256_mul_ps(delta, delta));
    }
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, total);
  float sum = Scalar<kDot>(a.subspan(i), b.subspan(i));
  for (float lane : lanes) {
    sum += lane;
  }
  return sum;
}
#elif defined(__aarch64__)
template <bool kDot>
float Simd(std::span<const float> a, std::span<const float> b) {
  auto total = vdupq_n_f32(0);
  std::size_t i = 0;
  for (; i + 4 <= a.size(); i += 4) {
    const auto x = vld1q_f32(a.data() + i);
    const auto y = vld1q_f32(b.data() + i);
    if constexpr (kDot) {
      total = vmlaq_f32(total, x, y);
    } else {
      const auto delta = vsubq_f32(x, y);
      total = vmlaq_f32(total, delta, delta);
    }
  }
  return vaddvq_f32(total) + Scalar<kDot>(a.subspan(i), b.subspan(i));
}
#endif

template <bool kDot>
auto Select() {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) {
    return &Simd<kDot>;
  }
#elif defined(__aarch64__)
  return &Simd<kDot>;
#endif
  return &Scalar<kDot>;
}

}  // namespace

bool ValidVector(std::span<const float> values) {
  return !values.empty() && values.size() <= 4096 &&
         std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value) && std::abs(value) <= 1e15f; });
}

std::vector<float> Prepare(std::span<const float> values, Metric metric) {
  std::vector<float> result(values.begin(), values.end());
  if (metric == Metric::kCosine) {
    double squared = 0;
    for (double value : values) {
      squared += value * value;
    }
    if (squared > 0) {
      const auto norm = std::sqrt(squared);
      for (auto& value : result) {
        value = static_cast<float>(value / norm);
      }
    }
  }
  return result;
}

float ScalarL2Squared(std::span<const float> a, std::span<const float> b) {
  return Scalar<false>(a, b);
}
float ScalarDot(std::span<const float> a, std::span<const float> b) { return Scalar<true>(a, b); }

float L2Squared(std::span<const float> a, std::span<const float> b) {
  static const auto kernel = Select<false>();
  return kernel(a, b);
}

float Dot(std::span<const float> a, std::span<const float> b) {
  static const auto kernel = Select<true>();
  return kernel(a, b);
}

float Distance(std::span<const float> a, std::span<const float> b, Metric metric) {
  return metric == Metric::kCosine ? std::clamp(1.0f - Dot(a, b), 0.0f, 2.0f) : L2Squared(a, b);
}

float ReportDistance(float distance, Metric metric) {
  return metric == Metric::kL2 ? std::sqrt(distance) : distance;
}

}  // namespace strata::vector
