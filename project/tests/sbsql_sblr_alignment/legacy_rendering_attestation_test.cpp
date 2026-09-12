// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostic_rendering/diagnostic_rendering.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace rendering = scratchbird::server::legacy_rendering;
namespace api = scratchbird::engine::internal_api;

namespace {
unsigned checks = 0;
unsigned failures = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
template<class T> void CheckNoAttestations() {
  Check(!requires(T value) { value.canonical_diagnostics; },
        "projection exposes fabricated canonical diagnostic attestation");
  Check(!requires(T value) { value.canonical_result_shape; },
        "projection exposes fabricated canonical result attestation");
}
// The fallback permits executing this regression against the original API.
// It does not add a production alias or grant either validator semantic credit.
template<class T> bool Structure(const T& value, std::vector<std::string>* errors) {
  if constexpr (requires { ValidateLegacyRenderedProjectionStructure(value, errors); }) {
    return ValidateLegacyRenderedProjectionStructure(value, errors);
  } else {
    return ValidateEngineRenderedResultEnvelope(value, errors);
  }
}
}

int main() {
  CheckNoAttestations<rendering::EngineRenderedResultEnvelope>();
  api::EngineApiResult source;
  source.ok = true;
  source.operation_id = "projection.component.fixture";
  source.result_shape.result_kind = "component_row";
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "text";
  value.encoded_value = "actual source bytes";
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "019e150f-0000-7000-8000-000000000015";
  row.fields.push_back({"value", value});
  source.result_shape.rows.push_back(row);
  source.evidence.push_back({"fixture_origin", "component_only"});
  rendering::EngineParserPackageRenderOptions options;
  options.parser_package_uuid = "019e150f-0000-7000-8000-000000000021";
  options.parser_package_version = "attestation-regression";
  options.client_dialect = "sbsql";
  const auto projected = rendering::RenderEngineApiResultForParserPackage(source, options);
  std::vector<std::string> errors;
  Check(Structure(projected, &errors), "well-formed projection structure rejected");
  Check(errors.empty(), "valid shape produced structure errors");
  Check(projected.ok == source.ok, "source operation outcome changed");
  Check(projected.operation_id == source.operation_id, "operation identity changed");
  Check(projected.rows.size() == 1, "source row lost");
  if (projected.rows.size() == 1) {
    Check(projected.rows[0].row_uuid == row.requested_row_uuid.canonical, "row identity changed");
    Check(projected.rows[0].fields.size() == 1, "source field lost");
    if (projected.rows[0].fields.size() == 1) {
      Check(projected.rows[0].fields[0].encoded_value == value.encoded_value,
            "projection fabricated a result value");
    }
  }
  Check(projected.evidence.size() == 1 && projected.evidence[0].evidence_kind == "fixture_origin"
        && projected.evidence[0].evidence_id == "component_only", "source evidence changed");
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    auto copy = projected;
    if (invalid == 0) copy.parser_package_uuid.clear();
    if (invalid == 1) copy.parser_package_version.clear();
    if (invalid == 2) copy.operation_id.clear();
    if (invalid == 3) copy.parser_finality_authority = true;
    if (invalid == 4) copy.reference_finality_authority = true;
    errors.clear();
    Check(!Structure(copy, &errors), "malformed projection structure accepted");
    Check(!errors.empty(), "structure rejection omitted reason");
  }
  std::cout << "legacy_projection_structure checks=" << checks
            << " failures=" << failures << " canonical_acceptance=0\n";
  return failures ? 1 : 0;
}
