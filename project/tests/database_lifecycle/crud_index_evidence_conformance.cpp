// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/crud_index_evidence.hpp"
#include "typed_update_carrier_codec.hpp"
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace wire = scratchbird::wire;

int main() try {
  const auto require = [](bool ok) {
    if (!ok) throw std::runtime_error("native index evidence oracle failed");
  };
  api::EngineUuid index{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,1}};
  api::BinaryCatalogMetadata fields;
  fields.identities = {{"index_uuid", index}};
  fields.text = {{"index_family","btree"}, {"index_profile","btree"},
      {"predicate_kind","column_equals"}, {"candidate_count","2"},
      {"visible_count","1"}, {"row_recheck_applied","true"},
      {"exactness","exact"}, {"fallback_mode","none"}};
  const auto encode = [&](const auto& f) {
    std::string bytes;
    require(api::EncodeBinaryCatalogMetadata(f, "crud.index_evidence.v2", &bytes));
    return bytes;
  };
  const auto bytes = encode(fields);
  std::vector<api::EngineEvidenceReference> output;
  api::AppendCrudIndexEvidence(&output, bytes);
  require(output.size() == 9 && output[0].evidence_kind == "index_lookup" &&
          std::get<api::EngineUuid>(output[0].evidence_id) == index);
  for (const auto& [name, value] : fields.text) {
    const auto found = std::find_if(output.begin(), output.end(), [&](const auto& row) {
      return row.evidence_kind == "index_lookup." + name;
    });
    require(found != output.end() && std::get<std::string>(found->evidence_id) == value);
  }
  std::size_t rejected = 0;
  const auto reject = [&](const std::string& malformed) {
    std::vector<api::EngineEvidenceReference> retained{{"sentinel", index}};
    bool refused = false;
    try { api::AppendCrudIndexEvidence(&retained, malformed); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused && retained.size() == 1 && retained[0].evidence_kind == "sentinel" &&
            std::get<api::EngineUuid>(retained[0].evidence_id) == index);
    ++rejected;
  };
  for (std::size_t n = 0; n < bytes.size(); ++n) reject(bytes.substr(0,n));
  reject(bytes + "x");
  auto invalid = bytes;
  invalid.replace(invalid.size()-16, 16, 16, '\0'); reject(invalid);
  invalid = bytes; invalid[invalid.size()-10] = 0x40; reject(invalid);
  invalid = bytes; invalid[invalid.size()-8] = 0; reject(invalid);
  auto changed = fields; changed.identities.clear(); reject(encode(changed));
  changed = fields; changed.identities.emplace("other_uuid", index); reject(encode(changed));
  changed = fields; changed.text.erase("candidate_count"); reject(encode(changed));
  changed = fields; changed.text.emplace("extra", "x"); reject(encode(changed));
  changed = fields; changed.text.erase("visible_count"); changed.text.emplace("wrong", "1"); reject(encode(changed));
  for (const auto& [name, unused] : fields.text) {
    for (const auto& value : {std::string{}, std::string("x\0y",3), std::string(1, char(0xff))}) {
      changed = fields; changed.text[name] = value; reject(encode(changed));
    }
  }
  std::vector<wire::TypedUpdateResultEvidenceReference> refs;
  for (const auto& row : output) {
    wire::TypedUpdateResultEvidenceReference ref;
    ref.evidence_kind = row.evidence_kind;
    if (const auto* id = std::get_if<api::EngineUuid>(&row.evidence_id)) ref.identity = id->bytes;
    else ref.evidence_id = std::get<std::string>(row.evidence_id);
    refs.push_back(std::move(ref));
  }
  wire::TypedUpdateResultCarrier result;
  result.update_descriptor_uuid = result.operation_uuid = result.owning_transaction_uuid = result.relation_uuid = index.bytes;
  wire::TypedUpdateHash effect{}, executor{}, changed_effect{}, changed_executor{};
  wire::TypedUpdateCarrierError error;
  require(wire::ComputeTypedUpdateResultInnerEvidence(result, refs, &effect, &executor, &error));
  auto altered = refs; altered[0].identity->back() ^= 2;
  require(wire::ComputeTypedUpdateResultInnerEvidence(result, altered, &changed_effect, &changed_executor, &error));
  require(effect != changed_effect && executor != changed_executor);
  altered = refs; altered.back().evidence_id += "changed";
  require(wire::ComputeTypedUpdateResultInnerEvidence(result, altered, &changed_effect, &changed_executor, &error));
  require(effect != changed_effect && executor != changed_executor);
  altered = {{"index_lookup", bytes, {}}};
  require(!wire::ComputeTypedUpdateResultInnerEvidence(result, altered, &changed_effect, &changed_executor, &error));
  require(error.code == wire::TypedUpdateCarrierErrorCode::result_evidence_material_invalid);
  output.clear(); api::AppendCrudIndexEvidence(&output, bytes, "transactional_index_resolution_evidence");
  require(output.size() == 9 && std::get<api::EngineUuid>(output[0].evidence_id) == index);
  std::cout << "native_index_evidence=passed rejected=" << rejected << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n'; return 1;
}
