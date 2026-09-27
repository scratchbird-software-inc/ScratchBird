// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/document_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <array>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  using scratchbird::tests::FixtureUuid;
  a::EngineRequestContext context;
  context.database_uuid = FixtureUuid(1148, 1);
  context.current_schema_uuid = FixtureUuid(1148, 2);
  a::PhysicalDocumentRecord record;
  record.collection_uuid = context.current_schema_uuid;
  record.document_uuid = FixtureUuid(1148, 3);
  record.document_uuid.bytes[10] = 0; record.document_uuid.bytes[15] = 0xff;
  record.row_uuid = FixtureUuid(1148, 4);
  record.version_uuid = FixtureUuid(1148, 5);
  record.name = "document"; record.payload = "opaque\ntext"; record.creator_tx = 42;
  const std::string raw(reinterpret_cast<const char*>(record.document_uuid.bytes.data()), 16);
  record.fragments = {{"document_uuid", raw}, {"value", "a\tb\nc"}, {"nil", ""}};
  record.fragment_types = {{"document_uuid", "uuid"}, {"value", "string"}, {"nil", "uuid"}};
  record.null_paths = {"nil"};
  std::string encoded, verb;
  Check(a::EncodeDocumentProviderEvent(context, "UPSERT", record, &encoded));
  Check(encoded.find(raw) != std::string::npos);
  a::PhysicalDocumentRecord decoded;
  Check(a::DecodeDocumentProviderEvent(context, encoded, &verb, &decoded));
  Check(verb == "UPSERT" && decoded.document_uuid == record.document_uuid &&
        decoded.version_uuid == record.version_uuid && decoded.fragments == record.fragments &&
        decoded.null_paths == record.null_paths && decoded.creator_tx == 42 &&
        decoded.fragment_types == record.fragment_types);
  Check(!a::DecodeDocumentProviderEvent(context, encoded.substr(0, encoded.size() - 1), &verb, &decoded));
  Check(!a::DecodeDocumentProviderEvent(context, "SBNOSQLDOC1\tUPSERT\tx\n", &verb, &decoded));
  auto wrong = context; wrong.database_uuid = FixtureUuid(1148, 6);
  Check(a::StoreKey(wrong) != a::StoreKey(context));
  Check(!a::DecodeDocumentProviderEvent(wrong, encoded, &verb, &decoded));
  a::BinaryCatalogMetadata fields;
  Check(a::DecodeBinaryCatalogMetadata(encoded, "nosql.document.event.v3", &fields));
  fields.text["creator_tx"] = "42junk";
  std::string corrupt;
  Check(a::EncodeBinaryCatalogMetadata(fields, "nosql.document.event.v3", &corrupt));
  Check(!a::DecodeDocumentProviderEvent(context, corrupt, &verb, &decoded));
  a::DocumentProviderState state; state.documents.emplace(record.document_uuid, record);
  const auto first = a::ProviderRowsFromState(state, record.collection_uuid);
  const auto second = a::ProviderRowsFromState(state, record.collection_uuid);
  Check(first.size() == 1 && second.size() == 1 && first[0].version_uuid == second[0].version_uuid &&
        first[0].version_uuid == record.version_uuid);
  a::EngineDocumentFindRequest request; request.typed_rows_only = true;
  request.projected_paths = {"document_uuid"};
  a::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = FixtureUuid(1148, 7); descriptor.canonical_type_name = "uuid";
  request.descriptors = {descriptor};
  a::DocumentPathProviderCandidate candidate;
  candidate.document_uuid = record.document_uuid; candidate.row_uuid = record.row_uuid;
  candidate.version_uuid = record.version_uuid;
  candidate.projected_values = {{"document_uuid", {"uuid", raw, false}}};
  a::EngineDocumentFindResult result; std::size_t cells = 0; std::uint64_t bytes = 0; bool refused = false;
  Check(a::AddProjectedDocumentRow(&result, candidate, request, &cells, &bytes, &refused));
  const auto& value = result.typed_rows[0].values[0].value;
  Check(value.encoded_value.empty() && value.binary_value.size() == 16 &&
        std::equal(value.binary_value.begin(), value.binary_value.end(), record.document_uuid.bytes.begin()));
  candidate.projected_values[0].value.encoded_value = "019f0000-0000-7000-8000-000000000001";
  Check(!a::AddProjectedDocumentRow(&result, candidate, request, &cells, &bytes, &refused));
  a::EngineApiRequest insert; insert.assignments = {{"document_uuid", value}};
  std::set<std::string> nulls; bool valid = false;
  std::map<std::string, std::string> types;
  Check(a::ParsePayloadFragments(insert, &nulls, &valid, &types).at("document_uuid") == raw && valid &&
        types.at("document_uuid") == "uuid");
  insert.assignments[0].second.binary_value.clear();
  insert.assignments[0].second.encoded_value = "019f0000-0000-7000-8000-000000000001";
  a::ParsePayloadFragments(insert, &nulls, &valid, &types); Check(!valid);
  // User UUID data is not a system identity: retain every bit, including nil,
  // v4, maximum and a non-RFC all-bit pattern, through codec and projection.
  std::array<a::EngineUuid, 4> user_values{};
  user_values[1].bytes[6] = 0x40; user_values[1].bytes[8] = 0x80;
  user_values[1].bytes[15] = 1;
  user_values[2].bytes.fill(0xff);
  for (std::size_t i = 0; i < 16; ++i) user_values[3].bytes[i] = static_cast<std::uint8_t>(i * 17);
  for (const auto& data : user_values) {
    a::EngineTypedValue cell;
    cell.descriptor = descriptor;
    cell.binary_value.assign(data.bytes.begin(), data.bytes.end());
    insert.assignments = {{"user_uuid", cell}};
    auto copy = record;
    copy.fragments = a::ParsePayloadFragments(insert, &copy.null_paths, &valid, &copy.fragment_types);
    Check(valid && a::EncodeDocumentProviderEvent(context, "UPSERT", copy, &encoded));
    Check(a::DecodeDocumentProviderEvent(context, encoded, &verb, &decoded) &&
          decoded.fragments == copy.fragments && decoded.fragment_types == copy.fragment_types);
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      a::BinaryCatalogMetadata malformed;
      Check(a::DecodeBinaryCatalogMetadata(encoded, "nosql.document.event.v3", &malformed));
      std::string fragments;
      a::AppendBinaryU32(&fragments, 1);
      Check(a::AppendBinaryString(&fragments, "user_uuid"));
      auto raw_value = copy.fragments.at("user_uuid");
      if (mutation == 0) raw_value.pop_back();
      Check(a::AppendBinaryString(&fragments, raw_value));
      Check(a::AppendBinaryString(&fragments, mutation == 1 ? "" : "uuid"));
      fragments.push_back(mutation == 2 ? 1 : mutation == 3 ? 2 : 0);
      malformed.text["fragments"] = fragments;
      std::string corrupt_scalar;
      Check(a::EncodeBinaryCatalogMetadata(malformed, "nosql.document.event.v3", &corrupt_scalar));
      Check(!a::DecodeDocumentProviderEvent(context, corrupt_scalar, &verb, &decoded));
    }
    a::DocumentProviderState typed_state; typed_state.documents.emplace(copy.document_uuid, decoded);
    const auto rows = a::ProviderRowsFromState(typed_state, copy.collection_uuid);
    Check(rows.size() == 1 && rows[0].values[0].value.scalar_type == "uuid" &&
          rows[0].values[0].value.encoded_value == copy.fragments.at("user_uuid"));
    candidate.projected_values = {{"document_uuid", rows[0].values[0].value}};
    result = {}; cells = 0; bytes = 0;
    Check(a::AddProjectedDocumentRow(&result, candidate, request, &cells, &bytes, &refused));
    Check(result.typed_rows[0].values[0].value.binary_value == cell.binary_value &&
          result.typed_rows[0].values[0].value.encoded_value.empty());
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
      auto malformed = cell;
      if (mutation == 0) malformed.binary_value.pop_back();
      if (mutation == 1) malformed.binary_value.push_back(0);
      if (mutation == 2) malformed.encoded_value = "conflicting text";
      if (mutation == 3) malformed.setState(a::EngineValueState::sql_null);
      if (mutation == 4) malformed.setState(a::EngineValueState::missing);
      insert.assignments = {{"user_uuid", malformed}};
      (void)a::ParsePayloadFragments(insert, &nulls, &valid, &types); Check(!valid);
    }
    copy.fragment_types.erase("user_uuid");
    Check(!a::EncodeDocumentProviderEvent(context, "UPSERT", copy, &encoded));
    copy = record; copy.document_uuid = data;
    Check(!a::EncodeDocumentProviderEvent(context, "UPSERT", copy, &encoded));
  }
  char path[] = "/tmp/sb_document_native_XXXXXX";
  const auto directory = ::mkdtemp(path); Check(directory != nullptr);
  context.database_path = std::string(directory) + "/database";
  Check(a::PersistDocumentProviderEvent(context, "UPSERT", record));
  a::DocumentProviderState loaded;
  Check(a::LoadDocumentProviderLocked(context, &loaded));
  Check(loaded.documents.size() == 1 && loaded.documents.begin()->second.version_uuid == record.version_uuid);
  Check(a::PersistDocumentProviderEvent(context, "DELETE", record));
  loaded = {}; Check(a::LoadDocumentProviderLocked(context, &loaded) && loaded.documents.empty());
  const auto disk = a::DocumentProviderPath(context);
  { std::ofstream out(disk, std::ios::binary | std::ios::app); out.put(1); }
  loaded = {}; Check(!a::LoadDocumentProviderLocked(context, &loaded) && !loaded.loaded && loaded.documents.empty());
  { std::ofstream out(disk, std::ios::binary | std::ios::trunc); out << "SBNOSQLDOC1\tUPSERT\tx\n"; }
  Check(!a::PersistDocumentProviderEvent(context, "UPSERT", record));
  Check(!a::LoadDocumentProviderLocked(context, &loaded));
  std::filesystem::remove(disk); std::filesystem::remove(directory);
}
