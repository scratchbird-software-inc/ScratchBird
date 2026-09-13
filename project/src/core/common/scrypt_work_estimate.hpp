// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <limits>

namespace scratchbird::core::crypto {
struct ScryptWorkEstimate {
  std::uint64_t workspace_bytes = 0, salsa208_calls = 0, sha256_blocks = 0, work_units = 0;
};
enum class ScryptEstimateCode { ok, invalid_parameters, overflow };
namespace scrypt_cost_detail {
inline bool Add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
  out = a + b; return true;
}
inline bool Mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
  out = a * b; return true;
}
inline std::uint64_t Ceil(std::uint64_t n, std::uint64_t d) noexcept { return n / d + (n % d != 0); }

} // namespace scrypt_cost_detail
inline ScryptEstimateCode EstimateScryptWork(std::uint64_t password_bytes, std::uint64_t salt_bytes,
    std::uint64_t n, std::uint64_t r, std::uint64_t p, std::uint64_t output_bytes, ScryptWorkEstimate& out) noexcept {
  using namespace scrypt_cost_detail;
  // Failure never leaves a plausible partial cost for accidental admission.
  out = {};
  if (n < 2 || (n & (n - 1)) || !r || !p || r > 0xffffffffULL || p > 0xffffffffULL ||
      !output_bytes || output_bytes > 65535 || r > ((std::uint64_t{1} << 30) - 1) / p ||
      (r < 4 && n >= (std::uint64_t{1} << (16 * r)))) return ScryptEstimateCode::invalid_parameters;
  ScryptWorkEstimate value;
  std::uint64_t rp, rows, row_bytes, blocks, initial, final, factor;
  const auto password_blocks = Ceil(password_bytes, 64);
  const auto salt_blocks = Ceil(salt_bytes, 64);
  if (!Mul(r,p,rp) || !Add(n,p,rows) || !Add(rows,2,rows) || !Mul(128,r,row_bytes) ||
      !Mul(rows,row_bytes,value.workspace_bytes) || !Mul(4,rp,blocks) ||
      !Mul(blocks,n,value.salsa208_calls) || !Add(password_blocks,salt_blocks,factor) ||
      !Add(factor,6,factor) || !Mul(blocks,factor,initial) ||
      !Mul(2,rp,factor) || !Add(factor,password_blocks,factor) || !Add(factor,6,factor) ||
      !Mul(Ceil(output_bytes,32),factor,final) || !Add(initial,final,value.sha256_blocks) ||
      !Add(value.salsa208_calls,value.sha256_blocks,value.work_units)) return ScryptEstimateCode::overflow;
  out = value;
  return ScryptEstimateCode::ok;
}

} // namespace scratchbird::core::crypto
