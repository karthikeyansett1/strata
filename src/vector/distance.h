#pragma once

#include <span>
#include <vector>

#include "strata/vector_store.h"

namespace strata::vector {

bool ValidVector(std::span<const float> values);
std::vector<float> Prepare(std::span<const float> values, Metric metric);
float ScalarL2Squared(std::span<const float> a, std::span<const float> b);
float ScalarDot(std::span<const float> a, std::span<const float> b);
float L2Squared(std::span<const float> a, std::span<const float> b);
float Dot(std::span<const float> a, std::span<const float> b);
float Distance(std::span<const float> prepared_a, std::span<const float> prepared_b, Metric metric);
float ReportDistance(float distance, Metric metric);

}  // namespace strata::vector
