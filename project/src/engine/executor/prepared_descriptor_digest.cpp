// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "prepared_execution_template.hpp"
#include "../../core/hash/hash_digest.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::executor {
namespace {

// Versioned content encoding, not an identity representation or allocator.
// UUIDs enter the hash as their sixteen canonical bytes. Variable data has
// uint64 little-endian length framing, including embedded NULs/delimiters.
class PreparedContentEncoder {
 public:
  explicit PreparedContentEncoder(std::uint8_t domain) : bytes_{'S', 'B', 'P', 'D', 1, domain} {}

  void Number(std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>(value));
      value >>= 8;
    }
  }

  void Text(std::string_view value) {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    Number(value.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  void Uuid(const internal_api::EngineUuid& value) {
    bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
  }

  void Descriptor(const internal_api::EngineDescriptor& value) {
    Uuid(value.descriptor_uuid);
    Uuid(value.type_uuid);
    Uuid(value.collation_uuid);
    Text(value.descriptor_kind);
    Text(value.canonical_type_name);
    Text(value.encoded_descriptor);
  }

  std::string Digest() const {
    const auto digest = core::hash::ComputeSha256Digest(bytes_);
    if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes) {
      throw PreparedContentHashFailure{};
    }
    return "sha256:" + core::hash::HexLower(digest.digest);
  }

 private:
  std::vector<core::platform::byte> bytes_;
};

}  // namespace

std::string PreparedTemplateStableDigest(const std::vector<std::string>& parts) {
  PreparedContentEncoder encoded(1);
  encoded.Number(parts.size());
  for (const auto& part : parts) encoded.Text(part);
  return encoded.Digest();
}

std::string PreparedDescriptorSetDigest(
    const std::vector<internal_api::EngineDescriptor>& descriptors,
    const std::vector<internal_api::EngineColumnDefinition>& columns) {
  PreparedContentEncoder encoded(2);
  encoded.Number(descriptors.size());
  for (const auto& descriptor : descriptors) encoded.Descriptor(descriptor);
  encoded.Number(columns.size());
  for (const auto& column : columns) {
    encoded.Uuid(column.requested_column_uuid);
    encoded.Number(column.ordinal);
    encoded.Descriptor(column.descriptor);
    encoded.Number(column.nullable ? 1 : 0);
  }
  return encoded.Digest();
}

std::string PreparedResultShapeDigest(const PreparedResultShapeDescriptor& shape) {
  PreparedContentEncoder encoded(3);
  encoded.Text(shape.result_kind);
  encoded.Number(shape.columns.size());
  for (const auto& column : shape.columns) {
    encoded.Text(column.stable_name);
    encoded.Number(column.ordinal);
    encoded.Descriptor(column.descriptor);
  }
  return encoded.Digest();
}

std::string PreparedDependencyDigest(std::vector<PreparedUuid> dependencies) {
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
  PreparedContentEncoder encoded(4);
  encoded.Number(dependencies.size());
  for (const auto& id : dependencies) encoded.Uuid(id);
  return encoded.Digest();
}

std::string PreparedPinnedDescriptorDigest(
    const std::vector<PreparedPinnedDescriptorReference>& pins) {
  if (pins.empty()) return {};  // Absence of pinned metadata, not a hash failure.
  PreparedContentEncoder encoded(5);
  encoded.Number(pins.size());
  for (const auto& pin : pins) {
    encoded.Text(pin.cache_key);
    encoded.Uuid(pin.catalog_epoch_uuid);
    encoded.Uuid(pin.descriptor_uuid);
    encoded.Uuid(pin.object_uuid);
    encoded.Uuid(pin.index_uuid);
    encoded.Text(pin.descriptor_set_digest);
    encoded.Number(pin.catalog_epoch);
    encoded.Number(pin.security_epoch);
    encoded.Number(pin.resource_policy_epoch);
    encoded.Number(pin.name_resolution_epoch);
    encoded.Number(pin.stats_epoch);
    encoded.Text(pin.security_policy_identity);
    encoded.Text(pin.redaction_policy_identity);
    encoded.Number(pin.read_only_snapshot);
    encoded.Number(pin.security_recheck_required);
    encoded.Number(pin.visibility_recheck_required);
    encoded.Number(pin.finality_authority_cached);
  }
  return encoded.Digest();
}

std::string PreparedTemplateCanonicalKey(const PreparedTemplateKey& key) {
  PreparedContentEncoder encoded(6);
  encoded.Text(key.operation_id);
  encoded.Text(key.sblr_digest_or_trace_key);
  encoded.Uuid(key.catalog_epoch_uuid);
  encoded.Text(key.descriptor_set_digest);
  encoded.Text(key.pinned_descriptor_set_digest);
  encoded.Text(key.result_shape_digest);
  encoded.Number(key.epochs.catalog_epoch);
  encoded.Number(key.epochs.security_epoch);
  encoded.Number(key.epochs.policy_resource_epoch);
  encoded.Number(key.epochs.name_resolution_epoch);
  encoded.Text(PreparedDependencyDigest(key.dependency_uuids));
  return encoded.Digest();
}

std::string PreparedAuthorizationDigest(const PreparedUuid& principal, const PreparedUuid& role) {
  PreparedContentEncoder encoded(7);
  encoded.Uuid(principal);
  encoded.Uuid(role);
  return encoded.Digest();
}

}  // namespace scratchbird::engine::executor
