// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/engine/executor/prepared_execution_template.hpp"

#include <iostream>
#include <stdexcept>

namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {
unsigned checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor value;
  value.descriptor_uuid = {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,16}};
  value.type_uuid = {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,17}};
  value.collation_uuid = {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,18}};
  value.descriptor_kind = "scalar";
  value.canonical_type_name = "text";
  value.encoded_descriptor = "nullability=nullable";
  return value;
}

void CheckFraming() {
  // Independent literal vectors: SBPD, version=1, domain=1, uint64-LE
  // part count followed by uint64-LE byte length and bytes of each part.
  // The expected full digests were checked with openssl dgst -sha256.
  Require(ex::PreparedTemplateStableDigest({}) ==
      "sha256:c6fac0ed7fc4117c64f4113b6f67c605ce34bea26d408cf403023d2841412137",
      "empty part-vector encoding or SHA-256 differs from literal oracle");
  Require(ex::PreparedTemplateStableDigest({"a", "b"}) ==
      "sha256:a398642aaf858ae2960656d47569d58124c42a7e90033aa2f892b67d3f4a0a36",
      "nonempty part-vector encoding or SHA-256 differs from literal oracle");
  Require(ex::PreparedTemplateStableDigest({}) != ex::PreparedTemplateStableDigest({""}),
          "empty collection aliases an empty field");
  Require(ex::PreparedTemplateStableDigest({"a", "b"}) !=
          ex::PreparedTemplateStableDigest({std::string{'a', char(0xff), 'b'}}),
          "old separator framing collision survived");
  Require(ex::PreparedTemplateStableDigest({"a", "b"}) !=
          ex::PreparedTemplateStableDigest({std::string{'a', '\0', 'b'}}),
          "NUL field framing collision");
  Require(ex::PreparedTemplateStableDigest({"a", "b"}) !=
          ex::PreparedTemplateStableDigest({"b", "a"}), "ordered parts were sorted");
  for (unsigned byte = 0; byte != 256; ++byte) {
    const std::string middle(1, static_cast<char>(byte));
    Require(ex::PreparedTemplateStableDigest({"a" + middle, "b"}) !=
            ex::PreparedTemplateStableDigest({"a", middle + "b"}),
            "field boundary depends on its byte contents");
  }
}

void CheckDescriptors() {
  const auto original = Descriptor();
  const auto expected = ex::PreparedDescriptorSetDigest({original}, {});
  const ex::PreparedResultShapeDescriptor shape{"rows", {{"value", original, 0}}, {}};
  const auto expected_shape = ex::PreparedResultShapeDigest(shape);
  api::EngineColumnDefinition column;
  column.requested_column_uuid = original.descriptor_uuid;
  column.descriptor = original;
  column.ordinal = 2;
  column.nullable = true;
  const auto expected_column = ex::PreparedDescriptorSetDigest({}, {column});
  for (const auto member : {&api::EngineDescriptor::descriptor_uuid,
                            &api::EngineDescriptor::type_uuid,
                            &api::EngineDescriptor::collation_uuid}) {
    for (unsigned bit = 0; bit != 128; ++bit) {
      auto changed = original;
      (changed.*member).bytes[bit / 8] ^= 1u << (bit % 8);
      Require(ex::PreparedDescriptorSetDigest({changed}, {}) != expected,
              "descriptor-set digest omits binary identity bits");
      auto changed_shape = shape;
      changed_shape.columns[0].descriptor = changed;
      Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape,
              "result-shape digest omits binary identity bits");
      auto changed_column = column;
      changed_column.descriptor = changed;
      Require(ex::PreparedDescriptorSetDigest({}, {changed_column}) != expected_column,
              "column digest omits descriptor binary identity bits");
    }
  }
  for (const auto member : {&api::EngineDescriptor::descriptor_kind,
                            &api::EngineDescriptor::canonical_type_name,
                            &api::EngineDescriptor::encoded_descriptor}) {
    auto changed = original;
    (changed.*member).push_back('\0');
    Require(ex::PreparedDescriptorSetDigest({changed}, {}) != expected,
            "descriptor-set digest omits a variable field or embedded NUL");
    auto changed_shape = shape;
    changed_shape.columns[0].descriptor = changed;
    Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape,
            "result-shape digest omits a variable descriptor field");
  }
  auto left = original, right = original;
  left.descriptor_kind = "a:";
  left.canonical_type_name = "b";
  right.descriptor_kind = "a";
  right.canonical_type_name = ":b";
  Require(ex::PreparedDescriptorSetDigest({left}, {}) !=
          ex::PreparedDescriptorSetDigest({right}, {}),
          "colon-separated descriptor collision survived");
  Require(ex::PreparedDescriptorSetDigest({left, right}, {}) !=
          ex::PreparedDescriptorSetDigest({right, left}, {}),
          "descriptor order lost");
  Require(ex::PreparedDescriptorSetDigest({original}, {}) !=
          ex::PreparedDescriptorSetDigest({original, original}, {}),
          "descriptor multiplicity lost");
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = column;
    changed.requested_column_uuid.bytes[bit / 8] ^= 1u << (bit % 8);
    Require(ex::PreparedDescriptorSetDigest({}, {changed}) != expected_column,
            "column identity not bound in descriptor set");
  }
  auto changed_column = column;
  ++changed_column.ordinal;
  Require(ex::PreparedDescriptorSetDigest({}, {changed_column}) != expected_column,
          "column ordinal not bound");
  changed_column = column;
  changed_column.nullable = false;
  Require(ex::PreparedDescriptorSetDigest({}, {changed_column}) != expected_column,
          "column nullability not bound");
  Require(ex::PreparedDescriptorSetDigest({original}, {column}) != expected_column,
          "separate descriptor and column collections not framed");
  Require(ex::PreparedDescriptorSetDigest({}, {column, changed_column}) !=
          ex::PreparedDescriptorSetDigest({}, {changed_column, column}),
          "column ordering not bound");
  auto changed_shape = shape;
  changed_shape.result_kind += ':';
  Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape, "result kind not bound");
  changed_shape = shape;
  changed_shape.columns[0].stable_name.push_back('\0');
  Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape, "slot name not bound");
  changed_shape = shape;
  ++changed_shape.columns[0].ordinal;
  Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape, "slot ordinal not bound");
  changed_shape = shape;
  changed_shape.columns.push_back(shape.columns[0]);
  Require(ex::PreparedResultShapeDigest(changed_shape) != expected_shape, "slot multiplicity not bound");
  changed_shape.columns[1].ordinal = 1;
  const auto ordered_shape = ex::PreparedResultShapeDigest(changed_shape);
  std::swap(changed_shape.columns[0], changed_shape.columns[1]);
  Require(ex::PreparedResultShapeDigest(changed_shape) != ordered_shape, "slot ordering not bound");
  changed_shape = shape;
  changed_shape.digest = "caller-supplied-stale-digest";
  Require(ex::PreparedResultShapeDigest(changed_shape) == expected_shape,
          "caller cached digest replaced actual content authority");
  Require(expected != expected_column && expected != expected_shape && expected_column != expected_shape,
          "descriptor set and shape domains alias");
}

void CheckPreparedKeys() {
  const auto d = Descriptor();
  const std::vector<api::EngineUuid> ids{d.descriptor_uuid, d.type_uuid};
  const auto dependency = ex::PreparedDependencyDigest(ids);
  Require(dependency == ex::PreparedDependencyDigest({d.type_uuid, d.descriptor_uuid, d.type_uuid}),
          "dependency set does not canonicalize order/duplicates");
  const auto auth = ex::PreparedAuthorizationDigest(d.descriptor_uuid, d.type_uuid);
  Require(auth != ex::PreparedAuthorizationDigest(d.type_uuid, d.descriptor_uuid),
          "principal and role domains were commuted");
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = d.descriptor_uuid;
    changed.bytes[bit / 8] ^= 1u << (bit % 8);
    Require(dependency != ex::PreparedDependencyDigest({changed, d.type_uuid}), "dependency bits omitted");
    Require(auth != ex::PreparedAuthorizationDigest(changed, d.type_uuid), "principal bits omitted");
    changed = d.type_uuid;
    changed.bytes[bit / 8] ^= 1u << (bit % 8);
    Require(auth != ex::PreparedAuthorizationDigest(d.descriptor_uuid, changed), "role bits omitted");
  }
  ex::PreparedPinnedDescriptorReference pin;
  pin.catalog_epoch_uuid = d.collation_uuid; pin.descriptor_uuid = d.descriptor_uuid;
  pin.object_uuid = d.type_uuid; pin.index_uuid = d.collation_uuid;
  pin.cache_key = "key"; pin.descriptor_set_digest = "descriptors";
  pin.security_policy_identity = "security"; pin.redaction_policy_identity = "redaction";
  const auto pinned = ex::PreparedPinnedDescriptorDigest({pin});
  for (auto member : {&ex::PreparedPinnedDescriptorReference::catalog_epoch_uuid,
       &ex::PreparedPinnedDescriptorReference::descriptor_uuid,
       &ex::PreparedPinnedDescriptorReference::object_uuid, &ex::PreparedPinnedDescriptorReference::index_uuid}) {
    for (unsigned bit = 0; bit != 128; ++bit) {
      auto changed = pin;
      (changed.*member).bytes[bit / 8] ^= 1u << (bit % 8);
      Require(pinned != ex::PreparedPinnedDescriptorDigest({changed}), "pinned identity bits omitted");
    }
  }
  for (auto member : {&ex::PreparedPinnedDescriptorReference::catalog_epoch,
       &ex::PreparedPinnedDescriptorReference::security_epoch,
       &ex::PreparedPinnedDescriptorReference::resource_policy_epoch,
       &ex::PreparedPinnedDescriptorReference::name_resolution_epoch,
       &ex::PreparedPinnedDescriptorReference::stats_epoch}) {
    for (unsigned bit = 0; bit != 64; ++bit) {
      auto changed = pin; changed.*member ^= std::uint64_t{1} << bit;
      Require(pinned != ex::PreparedPinnedDescriptorDigest({changed}), "pinned epoch bits omitted");
    }
  }
  for (auto member : {&ex::PreparedPinnedDescriptorReference::cache_key,
       &ex::PreparedPinnedDescriptorReference::descriptor_set_digest,
       &ex::PreparedPinnedDescriptorReference::security_policy_identity,
       &ex::PreparedPinnedDescriptorReference::redaction_policy_identity}) {
    auto changed = pin; (changed.*member).push_back('\0');
    Require(pinned != ex::PreparedPinnedDescriptorDigest({changed}), "pinned text field or NUL omitted");
  }
  for (auto member : {&ex::PreparedPinnedDescriptorReference::read_only_snapshot,
       &ex::PreparedPinnedDescriptorReference::security_recheck_required,
       &ex::PreparedPinnedDescriptorReference::visibility_recheck_required,
       &ex::PreparedPinnedDescriptorReference::finality_authority_cached}) {
    auto changed = pin; changed.*member = !(changed.*member);
    Require(pinned != ex::PreparedPinnedDescriptorDigest({changed}), "pinned policy flag omitted");
  }
  ex::PreparedTemplateKey key;
  key.operation_id = "a|sblr=b"; key.sblr_digest_or_trace_key = "c";
  key.catalog_epoch_uuid = d.descriptor_uuid; key.dependency_uuids = ids;
  const auto digest = ex::PreparedTemplateCanonicalKey(key);
  auto changed = key; changed.operation_id = "a"; changed.sblr_digest_or_trace_key = "b|sblr=c";
  Require(digest != ex::PreparedTemplateCanonicalKey(changed), "old delimited cache key collision survived");
  changed = key; changed.catalog_epoch_uuid = d.type_uuid;
  Require(digest != ex::PreparedTemplateCanonicalKey(changed), "key catalog authority omitted");
  for (auto member : {&ex::PreparedTemplateKey::descriptor_set_digest,
                     &ex::PreparedTemplateKey::pinned_descriptor_set_digest,
                     &ex::PreparedTemplateKey::result_shape_digest}) {
    changed = key; changed.*member = "different";
    Require(digest != ex::PreparedTemplateCanonicalKey(changed), "key content digest omitted");
  }
  for (auto member : {&ex::PreparedTemplateEpochs::catalog_epoch, &ex::PreparedTemplateEpochs::security_epoch,
                     &ex::PreparedTemplateEpochs::policy_resource_epoch, &ex::PreparedTemplateEpochs::name_resolution_epoch}) {
    for (unsigned bit = 0; bit != 64; ++bit) {
      changed = key; changed.epochs.*member ^= std::uint64_t{1} << bit;
      Require(digest != ex::PreparedTemplateCanonicalKey(changed), "key epoch bits omitted");
    }
  }
}
}  // namespace

int main() try {
  CheckFraming();
  CheckDescriptors();
  CheckPreparedKeys();
  std::cout << "PASS " << checks << " prepared descriptor binary/content digest checks; not cache or runtime acceptance\n";
} catch (const std::exception& error) {
  std::cerr << "FAIL " << error.what() << '\n';
  return 1;
}
