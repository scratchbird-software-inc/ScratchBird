// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostic_rendering/diagnostic_rendering.hpp"
#include <array>
#include <iostream>
#include <type_traits>

namespace render = scratchbird::server::legacy_rendering;
using Uuid = render::RenderUuid;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool pass, const char* reason) {
  ++checks;
  if (!pass) { ++failures; if (failures < 12) std::cerr << reason << '\n'; }
}
Uuid Id(unsigned last = 1) {
  Uuid id{{0x01, 0x9e, 0x15, 0x0f, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0, 0}};
  id.bytes[15] = static_cast<std::uint8_t>(last);
  return id;
}
bool SystemUuid(const Uuid& id) {
  return (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
}
render::EngineParserPackageRenderOptions Options() {
  render::EngineParserPackageRenderOptions options;
  options.parser_package_uuid = Id();
  options.parser_package_version = "native-identity-test";
  return options;
}
render::LegacyRenderSource Source() {
  render::LegacyRenderSource source;
  source.ok = true;
  source.operation_id = "component.native.projection";
  source.result_kind = "row";
  render::EngineRenderedField field;
  field.name = "user_uuid";
  field.descriptor.canonical_type_name = "uuid";
  field.binary_value.assign(16, 0);
  source.rows.push_back({Id(2), {field}});
  return source;
}
}
int main() {
  static_assert(std::is_same_v<decltype(render::EngineRenderedRow::row_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(render::EngineParserPackageRenderOptions::parser_package_uuid), Uuid>);
  const auto base = Source();
  using OptionMember = Uuid render::EngineParserPackageRenderOptions::*;
  using ResultMember = Uuid render::EngineRenderedResultEnvelope::*;
  const std::array<OptionMember, 6> options_members{
      &render::EngineParserPackageRenderOptions::parser_package_uuid,
      &render::EngineParserPackageRenderOptions::correlation_uuid,
      &render::EngineParserPackageRenderOptions::request_uuid,
      &render::EngineParserPackageRenderOptions::session_uuid,
      &render::EngineParserPackageRenderOptions::database_uuid,
      &render::EngineParserPackageRenderOptions::transaction_uuid};
  const std::array<ResultMember, 6> result_members{
      &render::EngineRenderedResultEnvelope::parser_package_uuid,
      &render::EngineRenderedResultEnvelope::correlation_uuid,
      &render::EngineRenderedResultEnvelope::request_uuid,
      &render::EngineRenderedResultEnvelope::session_uuid,
      &render::EngineRenderedResultEnvelope::database_uuid,
      &render::EngineRenderedResultEnvelope::transaction_uuid};
  for (std::size_t member = 0; member < options_members.size(); ++member)
    for (std::size_t byte = 0; byte < 16; ++byte)
      for (unsigned value = 0; value < 256; ++value) {
        auto options = Options();
        auto id = Id(3); id.bytes[byte] = static_cast<std::uint8_t>(value);
        options.*options_members[member] = id;
        const auto result = render::RenderEngineApiResultForParserPackage(base, options);
        Check(result.*result_members[member] == id, "context UUID bytes changed");
        Check(result.ok == SystemUuid(id), "context UUID admission mismatch");
        Check(render::ValidateLegacyRenderedProjectionStructure(result, nullptr) == SystemUuid(id),
              "context UUID validator mismatch");
      }
  for (unsigned role = 0; role < 4; ++role)
    for (std::size_t byte = 0; byte < 16; ++byte)
      for (unsigned value = 0; value < 256; ++value) {
        auto source = base; auto id = Id(4);
        id.bytes[byte] = static_cast<std::uint8_t>(value);
        if (role == 0) source.rows.front().row_uuid = id;
        if (role == 1) source.transaction_uuid = id;
        if (role == 2) { source.primary_object_kind = "database"; source.primary_object_uuid = id; }
        if (role == 3) source.evidence.push_back({"native_source", id});
        const auto result = render::RenderEngineApiResultForParserPackage(source, Options());
        const auto observed = role == 0 ? result.rows.front().row_uuid :
            role == 1 ? result.transaction_uuid : role == 2 ? result.database_uuid :
            std::get<Uuid>(result.evidence.front().evidence_id);
        Check(observed == id, "source UUID bytes changed");
        Check(result.ok == SystemUuid(id), "source UUID admission mismatch");
      }
  using DescriptorMember = Uuid render::EngineRenderedDescriptor::*;
  const std::array<DescriptorMember, 5> descriptor_members{
      &render::EngineRenderedDescriptor::descriptor_uuid,
      &render::EngineRenderedDescriptor::type_uuid,
      &render::EngineRenderedDescriptor::collation_uuid,
      &render::EngineRenderedDescriptor::datatype_descriptor_uuid,
      &render::EngineRenderedDescriptor::charset_uuid};
  for (const auto member : descriptor_members)
    for (std::size_t byte = 0; byte < 16; ++byte)
      for (unsigned value = 0; value < 256; ++value) {
        auto source = base; auto id = Id(5);
        id.bytes[byte] = static_cast<std::uint8_t>(value);
        source.rows.front().fields.front().descriptor.*member = id;
        source.columns.push_back(source.rows.front().fields.front().descriptor);
        const auto result = render::RenderEngineApiResultForParserPackage(source, Options());
        Check(result.columns.front().*member == id &&
              result.rows.front().fields.front().descriptor.*member == id,
              "descriptor UUID changed or lost");
        Check(result.ok == SystemUuid(id), "descriptor UUID admission mismatch");
      }
  for (std::size_t byte = 0; byte < 16; ++byte)
    for (unsigned value = 0; value < 256; ++value) {
      auto source = base;
      source.rows.front().fields.front().binary_value[byte] = static_cast<std::uint8_t>(value);
      const auto result = render::RenderEngineApiResultForParserPackage(source, Options());
      Check(result.ok && result.rows.front().fields.front().binary_value ==
            source.rows.front().fields.front().binary_value, "user UUID data restricted or changed");
    }
  for (unsigned state = 0; state < 8; ++state) {
    auto source = base; auto& field = source.rows.front().fields.front();
    field.state = static_cast<render::LegacyRenderValueState>(state);
    field.is_null = state == 1;
    if (state >= 1 && state <= 4) field.binary_value.clear();
    const auto result = render::RenderEngineApiResultForParserPackage(source, Options());
    Check(result.ok && result.rows.front().fields.front().state == field.state &&
          result.rows.front().fields.front().is_null == field.is_null &&
          result.rows.front().fields.front().binary_value == field.binary_value,
          "value state or payload lost");
  }
  for (unsigned invalid = 0; invalid < 10; ++invalid) {
    auto source = base; auto options = Options();
    auto& field = source.rows.front().fields.front();
    if (invalid == 0) options.parser_package_uuid = {};
    if (invalid == 1) source.rows.front().row_uuid = {};
    if (invalid == 2) { source.primary_object_kind = "database"; source.primary_object_uuid = {}; }
    if (invalid == 3) { source.transaction_uuid = Id(6); options.transaction_uuid = Id(7); }
    if (invalid == 4) {
      source.primary_object_kind = "database"; source.primary_object_uuid = Id(6);
      options.database_uuid = Id(7);
    }
    if (invalid == 5) field.binary_value.resize(15);
    if (invalid == 6) field.binary_value.resize(17);
    if (invalid == 7) { field.binary_value.clear(); field.encoded_value.assign(16, '\0'); }
    if (invalid == 8) { field.state = render::LegacyRenderValueState::sql_null; field.is_null = true; }
    if (invalid == 9) field.state = static_cast<render::LegacyRenderValueState>(255);
    const auto result = render::RenderEngineApiResultForParserPackage(source, options);
    Check(!result.ok && !result.render_context_valid &&
          !render::ValidateLegacyRenderedProjectionStructure(result, nullptr),
          "invalid authority or value state admitted");
  }
  std::cout << "native_legacy_render_checks=" << checks << " failures=" << failures
            << " system_octet_cases=61440 user_octet_cases=4096 canonical_acceptance=0\n";
  return failures ? 1 : 0;
}
