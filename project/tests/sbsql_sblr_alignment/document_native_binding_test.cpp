// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/document_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
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
  record.null_paths = {"nil"};
  std::string encoded, verb;
  Check(a::EncodeDocumentProviderEvent(context, "UPSERT", record, &encoded));
  Check(encoded.find(raw) != std::string::npos);
  a::PhysicalDocumentRecord decoded;
  Check(a::DecodeDocumentProviderEvent(context, encoded, &verb, &decoded));
  Check(verb == "UPSERT" && decoded.document_uuid == record.document_uuid &&
        decoded.version_uuid == record.version_uuid && decoded.fragments == record.fragments &&
        decoded.null_paths == record.null_paths && decoded.creator_tx == 42);
  Check(!a::DecodeDocumentProviderEvent(context, encoded.substr(0, encoded.size() - 1), &verb, &decoded));
  Check(!a::DecodeDocumentProviderEvent(context, "SBNOSQLDOC1\tUPSERT\tx\n", &verb, &decoded));
  auto wrong = context; wrong.database_uuid = FixtureUuid(1148, 6);
  Check(a::StoreKey(wrong) != a::StoreKey(context));
  Check(!a::DecodeDocumentProviderEvent(wrong, encoded, &verb, &decoded));
  a::BinaryCatalogMetadata fields;
  Check(a::DecodeBinaryCatalogMetadata(encoded, "nosql.document.event.v2", &fields));
  fields.text["creator_tx"] = "42junk";
  std::string corrupt;
  Check(a::EncodeBinaryCatalogMetadata(fields, "nosql.document.event.v2", &corrupt));
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
  Check(a::ParsePayloadFragments(insert, {}, &nulls, &valid).at("document_uuid") == raw && valid);
  insert.assignments[0].second.binary_value.clear();
  insert.assignments[0].second.encoded_value = "019f0000-0000-7000-8000-000000000001";
  a::ParsePayloadFragments(insert, {}, &nulls, &valid); Check(!valid);
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
