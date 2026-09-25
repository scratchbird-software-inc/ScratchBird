// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr/sblr_projection_uuid_literals.hpp"
#include "query/projection_api.hpp"
#include <iostream>
namespace scratchbird::engine::internal_api {
bool QowReadCanonicalProjectionExpressionsV1(
    const EngineApiRequest&, std::uint64_t, std::vector<EngineProjectionExpression>*,
    std::size_t*, std::size_t*, std::string*, std::string*);
}
namespace scratchbird::tests {
inline bool CheckProjectionUuidLiteralCarriers() {
  namespace api = engine::internal_api;
  namespace s = engine::sblr;
  bool ok = true;
  auto check = [&](bool passed, const char* detail) {
    if (!passed) std::cerr << "UUID projection carrier: " << detail << '\n';
    ok = passed && ok;
  };
  auto read = [](const api::EngineApiRequest& request,
                 std::vector<api::EngineProjectionExpression>* expressions) {
    std::size_t nodes = 0, depth = 0;
    std::string reason, detail;
    return api::QowReadCanonicalProjectionExpressionsV1(
        request, 1, expressions, &nodes, &depth, &reason, &detail);
  };
  const auto catalog = core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!catalog.ok()) return false;
  const auto row = core::datatypes::LookupDatatypeCatalogRow(
      catalog.manifest, core::datatypes::CanonicalTypeId::uuid);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
  const auto descriptor = row.manifest.descriptor_rows.front().descriptor_uuid.value;
  // Nil, all-one, all individual bits and non-v7 UUID data must survive exactly.
  for (unsigned sample = 0; sample < 130; ++sample) {
    core::platform::Uuid value;
    if (sample == 1) value.bytes.fill(0xff);
    if (sample >= 2) value.bytes[(sample - 2) / 8] = 1u << ((sample - 2) % 8);
    s::SblrOperationEnvelope envelope;
    s::SblrOperand operand;
    operand.name = "projection_0_value";
    operand.type = "uuid";
    operand.ordinal = 1;
    operand.value_kind = s::SblrValueKind::literal_typed;
    operand.value_body.assign(descriptor.bytes.begin(), descriptor.bytes.end());
    operand.value_body.resize(24, 0);
    operand.value_body[16] = 16;
    operand.value_body.insert(operand.value_body.end(), value.bytes.begin(), value.bytes.end());
    envelope.operands.push_back(operand);
    api::EngineApiRequest request;
    request.option_envelopes = {"projection_0_expr_kind:literal", "projection_0_type:uuid",
                                "projection_0_is_null:false"};
    check(s::ProjectSblrUuidLiterals(envelope, &request), "admit exact scalar bytes");
    std::vector<api::EngineProjectionExpression> expressions;
    check(read(request, &expressions) && expressions.size() == 1 &&
          expressions[0].binary_value == std::vector<std::uint8_t>(value.bytes.begin(), value.bytes.end()) &&
          expressions[0].encoded_value.empty(), "preserve every data bit without text");
    if (sample != 0) continue;
    for (unsigned bad_case = 0; bad_case < 8; ++bad_case) {
      auto malformed = envelope;
      auto& bad = malformed.operands.front();
      switch (bad_case) {
        case 0: bad.value_body.pop_back(); break;
        case 1: bad.value_body.push_back(0); break;
        case 2: bad.value_body[0] ^= 1; break;
        case 3: bad.value_body[16] = 15; break;
        case 4: bad.value = "00000000-0000-0000-0000-000000000000"; break;
        case 5: bad.value_kind = s::SblrValueKind::uuid_ref; break;
        case 6: bad.ordinal = 2; break;
        case 7: malformed.operands.push_back(bad); malformed.operands.back().ordinal = 2; break;
      }
      api::EngineApiRequest target;
      check(!s::ProjectSblrUuidLiterals(malformed, &target) && target.projection.uuid_literals.empty(),
            "refuse malformed framing without publishing a partial value");
    }
    auto conflict = request;
    conflict.projection.uuid_literals.front().second.bytes[0] = 1;
    check(!s::ProjectSblrUuidLiterals(envelope, &conflict) &&
          conflict.projection.uuid_literals.front().second.bytes[0] == 1,
          "refuse conflict with bound request without overwriting it");
    for (unsigned bad_case = 0; bad_case < 7; ++bad_case) {
      auto bad = request;
      switch (bad_case) {
        case 0: bad.projection.uuid_literals.push_back(bad.projection.uuid_literals.front()); break;
        case 1: bad.projection.uuid_literals.front().first = "projection_1_"; break;
        case 2: bad.option_envelopes.push_back("projection_0_value:"); break;
        case 3: bad.option_envelopes[1] = "projection_0_type:character"; break;
        case 4: bad.option_envelopes[2] = "projection_0_is_null:true"; break;
        case 5: bad.projection.uuid_literals.clear(); break;
        case 6: bad.option_envelopes[0] = "projection_0_expr_kind:parameter"; break;
      }
      check(!read(bad, &expressions) && expressions.empty(), "reject conflicting or unused literal carrier");
    }
  }
  return ok;
}
} // namespace scratchbird::tests
