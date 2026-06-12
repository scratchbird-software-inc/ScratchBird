// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_transport_api.hpp"

#include "catalog/wire_driver_metadata_api.hpp"
#include "datatype_binary.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

std::string BoolText(bool value) { return value ? "1" : "0"; }

bool SupportedTransportScope(const std::string& scope) {
  return scope == "backup" || scope == "restore" || scope == "archive" || scope == "replication";
}

std::vector<scratchbird::core::platform::byte> BytesFromString(const std::string& value) {
  return {value.begin(), value.end()};
}

std::string StringFromBytes(const std::vector<scratchbird::core::platform::byte>& value) {
  return {value.begin(), value.end()};
}

scratchbird::core::datatypes::DatatypeDescriptorRecord Record(std::string name,
                                                               const std::string& value) {
  scratchbird::core::datatypes::DatatypeDescriptorRecord record;
  record.field_name = std::move(name);
  record.payload = BytesFromString(value);
  return record;
}

bool ReadRecord(const scratchbird::core::datatypes::DatatypeDescriptorEnvelope& envelope,
                const std::string& name,
                std::string* out) {
  for (const auto& record : envelope.records) {
    if (record.field_name == name) {
      *out = StringFromBytes(record.payload);
      return true;
    }
  }
  return false;
}

bool ReadBoolRecord(const scratchbird::core::datatypes::DatatypeDescriptorEnvelope& envelope,
                    const std::string& name,
                    bool* out) {
  std::string value;
  if (!ReadRecord(envelope, name, &value)) {
    return false;
  }
  if (value == "1") {
    *out = true;
    return true;
  }
  if (value == "0") {
    *out = false;
    return true;
  }
  return false;
}

EngineDatatypeTransportDecodeResult DecodeFailure(std::string detail) {
  EngineDatatypeTransportDecodeResult result;
  result.diagnostic_detail = std::move(detail);
  return result;
}

}  // namespace

EngineDatatypeTransportEncodeResult EncodeDatatypeTransportRecord(const EngineDatatypeTransportRecord& record) {
  EngineDatatypeTransportEncodeResult result;
  if (!SupportedTransportScope(record.transport_scope)) {
    result.diagnostic_detail = "unsupported_transport_scope";
    return result;
  }
  if (record.descriptor.canonical_type_name.empty() && record.descriptor.encoded_descriptor.empty()) {
    result.diagnostic_detail = "descriptor_required";
    return result;
  }
  const auto metadata = RenderWireDriverMetadata(record.descriptor, record.compatibility_dialect, record.reference_label);
  scratchbird::core::datatypes::DatatypeDescriptorEnvelope envelope;
  envelope.kind = scratchbird::core::datatypes::DatatypeDescriptorEnvelopeKind::datatype_transport;
  envelope.integrity_profile =
      scratchbird::core::datatypes::DatatypeDescriptorIntegrityProfile::strong;
  envelope.records = {
      Record("transport_scope", record.transport_scope),
      Record("descriptor_uuid", record.descriptor.descriptor_uuid.canonical),
      Record("descriptor_kind", record.descriptor.descriptor_kind),
      Record("descriptor_canonical_type_name", record.descriptor.canonical_type_name),
      Record("descriptor_encoded_descriptor", record.descriptor.encoded_descriptor),
      Record("value_descriptor_uuid", record.value.descriptor.descriptor_uuid.canonical),
      Record("value_descriptor_kind", record.value.descriptor.descriptor_kind),
      Record("value_descriptor_canonical_type_name", record.value.descriptor.canonical_type_name),
      Record("value_descriptor_encoded_descriptor", record.value.descriptor.encoded_descriptor),
      Record("value_encoded_value", record.value.encoded_value),
      Record("value_is_null", BoolText(record.value.is_null)),
      Record("compatibility_dialect", record.compatibility_dialect),
      Record("reference_label", record.reference_label),
      Record("reference_label_alias_only",
             BoolText(record.reference_label_alias_only && metadata.reference_label_alias_only)),
      Record("opaque_render_only", BoolText(record.opaque_render_only || metadata.opaque_render_only)),
  };
  const auto encoded = scratchbird::core::datatypes::EncodeDatatypeDescriptorEnvelope(envelope);
  if (!encoded.ok()) {
    result.diagnostic_detail = encoded.diagnostic.diagnostic_code;
    return result;
  }
  result.encoded_envelope.assign(reinterpret_cast<const char*>(encoded.encoded.data()),
                                 encoded.encoded.size());
  result.ok = true;
  return result;
}

EngineDatatypeTransportDecodeResult DecodeDatatypeTransportRecord(const std::string& encoded_envelope) {
  EngineDatatypeTransportDecodeResult result;
  const std::vector<scratchbird::core::platform::byte> bytes(encoded_envelope.begin(),
                                                            encoded_envelope.end());
  const auto decoded = scratchbird::core::datatypes::DecodeDatatypeDescriptorEnvelope(bytes);
  if (!decoded.ok() ||
      decoded.envelope.kind !=
          scratchbird::core::datatypes::DatatypeDescriptorEnvelopeKind::datatype_transport ||
      decoded.envelope.integrity_profile !=
          scratchbird::core::datatypes::DatatypeDescriptorIntegrityProfile::strong) {
    result.diagnostic_detail = decoded.ok() ? "bad_transport_envelope"
                                            : decoded.diagnostic.diagnostic_code;
    return result;
  }
  if (decoded.envelope.records.size() != 15) {
    return DecodeFailure("bad_transport_record_count");
  }

  if (!ReadRecord(decoded.envelope, "transport_scope", &result.record.transport_scope) ||
      !SupportedTransportScope(result.record.transport_scope)) {
    return DecodeFailure("bad_transport_scope");
  }
  if (!ReadRecord(decoded.envelope, "descriptor_uuid", &result.record.descriptor.descriptor_uuid.canonical)) {
    return DecodeFailure("bad_descriptor_uuid");
  }
  if (!ReadRecord(decoded.envelope, "descriptor_kind", &result.record.descriptor.descriptor_kind)) {
    return DecodeFailure("bad_descriptor_kind");
  }
  if (!ReadRecord(decoded.envelope, "descriptor_canonical_type_name",
                  &result.record.descriptor.canonical_type_name)) {
    return DecodeFailure("bad_canonical_type");
  }
  if (!ReadRecord(decoded.envelope, "descriptor_encoded_descriptor",
                  &result.record.descriptor.encoded_descriptor)) {
    return DecodeFailure("bad_encoded_descriptor");
  }
  if (!ReadRecord(decoded.envelope, "value_descriptor_uuid",
                  &result.record.value.descriptor.descriptor_uuid.canonical)) {
    return DecodeFailure("bad_value_descriptor_uuid");
  }
  if (!ReadRecord(decoded.envelope, "value_descriptor_kind",
                  &result.record.value.descriptor.descriptor_kind)) {
    return DecodeFailure("bad_value_descriptor_kind");
  }
  if (!ReadRecord(decoded.envelope, "value_descriptor_canonical_type_name",
                  &result.record.value.descriptor.canonical_type_name)) {
    return DecodeFailure("bad_value_canonical_type");
  }
  if (!ReadRecord(decoded.envelope, "value_descriptor_encoded_descriptor",
                  &result.record.value.descriptor.encoded_descriptor)) {
    return DecodeFailure("bad_value_encoded_descriptor");
  }
  if (!ReadRecord(decoded.envelope, "value_encoded_value", &result.record.value.encoded_value)) {
    return DecodeFailure("bad_value");
  }
  if (!ReadBoolRecord(decoded.envelope, "value_is_null", &result.record.value.is_null)) {
    return DecodeFailure("bad_value_is_null");
  }
  if (!ReadRecord(decoded.envelope, "compatibility_dialect", &result.record.compatibility_dialect)) {
    return DecodeFailure("bad_compatibility_dialect");
  }
  if (!ReadRecord(decoded.envelope, "reference_label", &result.record.reference_label)) {
    return DecodeFailure("bad_reference_label");
  }
  if (!ReadBoolRecord(decoded.envelope, "reference_label_alias_only",
                      &result.record.reference_label_alias_only)) {
    return DecodeFailure("bad_reference_label_alias_only");
  }
  if (!ReadBoolRecord(decoded.envelope, "opaque_render_only", &result.record.opaque_render_only)) {
    return DecodeFailure("bad_opaque_render_only");
  }
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
