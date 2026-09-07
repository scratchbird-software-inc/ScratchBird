// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_procedural_body_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

int Require(bool condition, int code) { return condition ? 0 : code; }

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;

  sblr::ProceduralBodyUuid body_uuid{};
  sblr::ProceduralBodyUuid procedure_uuid{};
  body_uuid[0] = 0x01;
  body_uuid[6] = 0x70;
  body_uuid[8] = 0x80;
  body_uuid[15] = 0x01;
  procedure_uuid[0] = 0x01;
  procedure_uuid[6] = 0x70;
  procedure_uuid[8] = 0x80;
  procedure_uuid[15] = 0x02;

  const auto body = sblr::MakeSblrPsqlNullProceduralBodyV1(
      body_uuid, 7, procedure_uuid, 11, 13);
  const auto canonical = sblr::EncodeSblrProceduralBodyV1(body);
  if (const auto failure =
          Require(canonical.size() == sblr::kSblrProceduralBodyV1Bytes, 1)) {
    return failure;
  }

  sblr::SblrProceduralBodyV1 decoded;
  std::string detail;
  if (const auto failure = Require(
          sblr::DecodeSblrProceduralBodyV1(canonical.data(), canonical.size(),
                                           &decoded, &detail),
          2)) {
    return failure;
  }
  if (const auto failure = Require(
          decoded.body_uuid == body_uuid && decoded.body_generation == 7 &&
              decoded.procedure_uuid == procedure_uuid &&
              decoded.procedure_generation == 11 &&
              decoded.node_executor_availability_generation == 13,
          3)) {
    return failure;
  }
  if (const auto failure =
          Require(sblr::EncodeSblrProceduralBodyV1(decoded) == canonical, 4)) {
    return failure;
  }

  const auto must_refuse = [&](std::vector<std::uint8_t> bytes, int code) {
    sblr::SblrProceduralBodyV1 ignored;
    detail.clear();
    return Require(!sblr::DecodeSblrProceduralBodyV1(
                       bytes.data(), bytes.size(), &ignored, &detail) &&
                       !detail.empty(),
                   code);
  };

  auto changed = canonical;
  changed[0] = 'X';
  if (const auto failure = must_refuse(changed, 5)) return failure;
  changed = canonical;
  changed[72] = 1;
  if (const auto failure = must_refuse(changed, 6)) return failure;
  changed = canonical;
  changed[80] = 2;
  if (const auto failure = must_refuse(changed, 7)) return failure;
  changed = canonical;
  std::fill(changed.begin() + 16, changed.begin() + 32, 0);
  if (const auto failure = must_refuse(changed, 8)) return failure;
  changed = canonical;
  changed[22] = 0x60;
  if (const auto failure = must_refuse(changed, 14)) return failure;
  changed = canonical;
  changed[96] ^= 0x01;
  if (const auto failure = must_refuse(changed, 9)) return failure;
  changed = canonical;
  changed[256] ^= 0x01;
  if (const auto failure = must_refuse(changed, 10)) return failure;
  changed = canonical;
  changed.pop_back();
  if (const auto failure = must_refuse(changed, 11)) return failure;

  auto invalid = body;
  invalid.node_executor_availability_generation = 0;
  if (const auto failure =
          Require(sblr::EncodeSblrProceduralBodyV1(invalid).empty(), 12)) {
    return failure;
  }
  invalid = body;
  invalid.node_descriptor_sha256[0] ^= 0x01;
  if (const auto failure =
          Require(sblr::EncodeSblrProceduralBodyV1(invalid).empty(), 13)) {
    return failure;
  }
  return 0;
}
