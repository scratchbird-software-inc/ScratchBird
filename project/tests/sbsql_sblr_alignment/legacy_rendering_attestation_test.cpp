// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostic_rendering/diagnostic_rendering.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"

#include <iostream>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace rendering = scratchbird::server::legacy_rendering;
namespace api = scratchbird::engine::internal_api;
namespace canonical = scratchbird::core::diagnostics;

namespace { long fail_after=-1; unsigned allocation_faults=0; }
void* operator new(std::size_t size) {
  if(fail_after==0)throw std::bad_alloc();
  if(fail_after>0)--fail_after;
  if(void* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

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
template<class T> void CheckNoInventedPolicy() {
  Check(!requires(T value){value.retryable;},"boolean retry policy remains inferred by renderer");
  Check(!requires(T value){value.public_shape_id;},"renderer invents public diagnostic shape");
  Check(!requires(T value){value.private_shape_id;},"renderer invents private diagnostic shape");
  Check(!requires(T value){value.recommended_action;},"renderer invents recommended action");
  Check(!requires(T value){value.redaction_class;},"renderer invents redaction policy class");
}
bool Same(const canonical::CanonicalDiagnosticMetadata& a,
          const canonical::CanonicalDiagnosticMetadata& b) {
  return a.code==b.code&&a.severity==b.severity&&a.is_failure==b.is_failure&&
      a.sqlstate==b.sqlstate&&a.numeric_binding==b.numeric_binding&&a.retry_class==b.retry_class&&
      a.required_outcome==b.required_outcome&&a.diagnostic_class==b.diagnostic_class;
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
  CheckNoInventedPolicy<rendering::EngineRenderedDiagnostic>();
  api::EngineApiResult source;
  source.ok = true;
  source.operation_id = "projection.component.fixture";
  source.result_shape.result_kind = "component_row";
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "text";
  value.encoded_value = "actual source bytes";
  api::EngineRowValue row;
  row.requested_row_uuid = {{0x01,0x9e,0x15,0x0f,0,0,0x70,0,0x80,0,0,0,0,0,0,0x15}};
  row.fields.push_back({"value", value});
  api::EngineTypedValue binary;
  binary.descriptor.descriptor_kind="scalar";
  binary.descriptor.canonical_type_name="uuid";
  binary.binary_value.assign(row.requested_row_uuid.bytes.begin(),row.requested_row_uuid.bytes.end());
  row.fields.push_back({"binary",binary});
  source.result_shape.rows.push_back(row);
  source.evidence.push_back({"fixture_origin", "component_only"});
  source.evidence.push_back({"binary_reference",row.requested_row_uuid});
  rendering::EngineParserPackageRenderOptions options;
  const api::EngineUuid package_uuid{{0x01,0x9e,0x15,0x0f,0,0,0x70,0,0x80,0,0,0,0,0,0,0x21}};
  options.parser_package_uuid.assign(reinterpret_cast<const char*>(package_uuid.bytes.data()), package_uuid.bytes.size());
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
    Check(projected.rows[0].row_uuid.size() == 16 &&
          projected.rows[0].row_uuid == std::string(
              reinterpret_cast<const char*>(row.requested_row_uuid.bytes.data()),16),
          "binary row identity changed");
    Check(projected.rows[0].fields.size() == 2, "source field lost");
    if (projected.rows[0].fields.size() == 2) {
      Check(projected.rows[0].fields[0].encoded_value == value.encoded_value,
            "projection fabricated a result value");
      Check(projected.rows[0].fields[1].binary_value==binary.binary_value &&
            projected.rows[0].fields[1].encoded_value.empty(),
            "projection dropped binary data or synthesized UUID text");
    }
  }
  Check(projected.evidence.size() == 2 && projected.evidence[0].evidence_kind == "fixture_origin"
        && std::get<std::string>(projected.evidence[0].evidence_id) == "component_only", "source evidence changed");
  Check(projected.evidence.size() == 2 &&
        std::get<api::EngineUuid>(projected.evidence[1].evidence_id)==row.requested_row_uuid,
        "binary source evidence changed or became text");
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
  for(const auto& registered:canonical::CanonicalDiagnosticCodeCatalog()) {
    api::EngineApiResult input;
    input.ok=!registered.is_failure;input.operation_id="component.diagnostic.projection";
    input.diagnostics.push_back(api::MakeEngineApiDiagnostic(std::string(registered.code),
        "component.source.key","FATAL WARNING timeout retry stale busy hidden internal route",registered.is_failure));
    const auto output=rendering::RenderEngineApiResultForParserPackage(input,options);
    Check(output.ok==input.ok&&output.render_context_valid,"registered source outcome changed");
    Check(output.diagnostics.size()==1,"registered source diagnostic lost");
    if(output.diagnostics.size()!=1)continue;
    const auto& d=output.diagnostics.front();const auto& s=input.diagnostics.front();
    Check(d.source_metadata&&s.canonical_metadata&&Same(*d.source_metadata,*s.canonical_metadata),
        "registry metadata inferred or changed");
    Check(d.occurrence_uuid==s.occurrence_uuid,"source occurrence identity replaced");
    Check(d.code==s.code&&d.message_key==s.message_key&&d.error==s.error,"source fields changed");
    Check(d.internal_detail_redacted&&d.detail=="redacted","hidden detail escaped legacy redaction");
    errors.clear();Check(Structure(output,&errors)&&errors.empty(),"registered source projection invalid");
  }
  auto valid_source=api::MakeEngineApiDiagnostic("SERVER.SHUTDOWN.DRAIN_TIMEOUT",
      "server.shutdown.drain_timeout","not a retry instruction",false);
  Check(valid_source.canonical_metadata.has_value(),"required registered shutdown diagnostic absent");
  for(unsigned invalid=0;invalid<10;++invalid) {
    auto d=valid_source;
    if(invalid==0)d.canonical_metadata.reset();
    if(invalid==1)d.canonical_metadata->code="other.code";
    if(invalid==2)d.canonical_metadata->severity=static_cast<canonical::CanonicalSeverity>(255);
    if(invalid==3)d.occurrence_uuid={};
    if(invalid==4)d.occurrence_uuid[6]=0x40;
    if(invalid==5)d.error=true;
    if(invalid==6)d=api::MakeEngineApiDiagnostic("SB_ENGINE_API_INVALID_REQUEST","invalid.request","private",true);
    if(invalid==7)d.message_key.clear();
    if(invalid==8){d.code.clear();d.canonical_metadata->code.clear();}
    if(invalid==9)d.canonical_metadata->severity=static_cast<canonical::CanonicalSeverity>(0);
    api::EngineApiResult input;input.ok=true;input.operation_id="invalid.source";input.diagnostics.push_back(d);
    const auto output=rendering::RenderEngineApiResultForParserPackage(input,options);
    errors.clear();Check(!output.ok&&!output.render_context_valid&&!Structure(output,&errors),
        "missing or malformed diagnostic authority admitted");
  }
  api::EngineApiResult input;input.ok=true;input.operation_id="allocation.projection";
  input.diagnostics.push_back(valid_source);
  bool complete=false;
  for(long site=0;site<1000;++site) {
    rendering::EngineRenderedResultEnvelope output;output.operation_id="sentinel";
    fail_after=site;
    try {
      output=rendering::RenderEngineApiResultForParserPackage(input,options);fail_after=-1;
      Check(output.ok&&output.diagnostics.size()==1,"allocation sweep completed with partial result");
      complete=true;break;
    }catch(const std::bad_alloc&){
      fail_after=-1;++allocation_faults;
      Check(output.operation_id=="sentinel"&&output.diagnostics.empty(),"allocation failure published partial result");
    }
  }
  Check(complete&&allocation_faults>0,"allocation sweep incomplete");
  std::cout << "legacy_projection_structure checks=" << checks
            << " failures=" << failures << " allocation_faults=" << allocation_faults << " canonical_acceptance=0\n";
  return failures ? 1 : 0;
}
