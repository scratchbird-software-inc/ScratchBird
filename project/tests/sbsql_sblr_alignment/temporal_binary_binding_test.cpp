// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/dml/temporal_bitemporal_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  a::EngineApiRequest request;
  request.target_object.object_kind = "temporal_period";
  request.target_object.uuid = scratchbird::tests::FixtureUuid(1100, 1);
  auto table = scratchbird::tests::FixtureUuid(1100, 2);
  table.bytes[9] = 0; table.bytes[15] = 0xff;
  a::EngineObjectReference reference;
  reference.object_kind = "table"; reference.uuid = table;
  request.related_objects.push_back(reference);
  request.option_envelopes = {"history_table:history;table_uuid=spoof", "application_time_from:one"};
  request.context.local_transaction_id = UINT64_MAX;
  request.context.snapshot_visible_through_local_transaction_id = UINT64_MAX - 1;
  Check(a::TableUuid(request) == table);
  Check(a::PeriodUuid(request) == request.target_object.uuid);
  const auto payload = a::TemporalPayload(request, "create_period", "application_time");
  Check(payload.has_value());
  a::BinaryCatalogMetadata decoded;
  Check(a::DecodeTemporalPayload(*payload, &decoded));
  Check(decoded.identities.size() == 2);
  Check(a::PayloadUuid(decoded, "table_uuid") == table);
  Check(a::PayloadUuid(decoded, "period_uuid") == request.target_object.uuid);
  Check(a::PayloadField(decoded, "history_table") == "history;table_uuid=spoof");
  Check(a::PayloadField(decoded, "local_transaction_id") == "18446744073709551615");
  Check(!decoded.text.contains("table_uuid") && !decoded.text.contains("period_uuid"));
  const auto saved = decoded.identities;
  Check(!a::DecodeTemporalPayload("table_uuid=018f0000-0000-7000-8000-000000000001", &decoded));
  Check(decoded.identities == saved);
  Check(!a::DecodeTemporalPayload(payload->substr(0, payload->size() - 1), &decoded));
  Check(!a::DecodeTemporalPayload(*payload + "x", &decoded));
  auto invalid = decoded;
  invalid.identities["table_uuid"].bytes[8] = 0;
  std::string invalid_bytes;
  Check(!a::EncodeBinaryCatalogMetadata(invalid, "temporal.v2", &invalid_bytes));
  invalid_bytes = *payload;
  const std::string table_bytes(reinterpret_cast<const char*>(table.bytes.data()), table.bytes.size());
  const auto table_offset = invalid_bytes.find(table_bytes);
  Check(table_offset != std::string::npos);
  invalid_bytes[table_offset + 8] = 0;
  Check(!a::DecodeTemporalPayload(invalid_bytes, &decoded));
  Check(decoded.identities == saved);
  invalid = decoded;
  invalid.text["table_uuid"] = "forbidden";
  Check(!a::EncodeBinaryCatalogMetadata(invalid, "temporal.v2", &invalid_bytes));
  Check(a::MatchesTarget(table, table, request));
  auto other = table; other.bytes[15] ^= 1;
  Check(!a::MatchesTarget(other, table, request));
  request.target_object.object_kind = "table";
  request.target_object.uuid = table;
  request.related_objects.clear();
  Check(a::PeriodUuid(request).is_nil() && a::TableUuid(request) == table);
}
