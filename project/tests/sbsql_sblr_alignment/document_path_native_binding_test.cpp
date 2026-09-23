// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/document_path_physical_provider.cpp"
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
  context.database_uuid = FixtureUuid(1146, 1);
  context.current_schema_uuid = FixtureUuid(1146, 2);
  char path[] = "/tmp/sb_docpath_native_XXXXXX";
  const auto directory = ::mkdtemp(path); Check(directory != nullptr);
  context.database_path = std::string(directory) + "/database";
  auto identity = a::DocumentPathProviderIdentityForContext(context, 1);
  Check(a::HasRequiredIdentity(identity));
  Check(identity.database_uuid == context.database_uuid);
  Check(identity.relation_uuid == context.current_schema_uuid);
  Check(a::DocumentPathProviderIdentityForContext(context, 2).index_uuid == identity.index_uuid);
  a::DocumentPathRowEvidence row;
  row.document_uuid = FixtureUuid(1146, 3); row.document_uuid.bytes[9] = 0; row.document_uuid.bytes[15] = 0xff;
  row.row_uuid = FixtureUuid(1146, 4); row.version_uuid = FixtureUuid(1146, 5);
  row.row_ordinal = 1;
  row.values = {{"profile.name", {"string", "binary-safe\nvalue", false}},
                {"items.0.count", {"int64", "7", false}}};
  a::DocumentPathProviderBuildRequest request;
  request.identity = identity; request.rows = {row};
  request.artifact_path = a::DocumentPathPhysicalProviderPath(context);
  const auto built = a::BuildDocumentPathPhysicalProvider(request);
  Check(built.ok);
  const auto bytes = a::SerializeArtifact(built.artifact);
  Check(!bytes.empty());
  a::DocumentPathProviderOpenRequest open;
  open.expected_identity = identity; open.require_expected_identity = true;
  const auto loaded = a::ParseArtifactBinary(bytes, open);
  Check(loaded.ok && loaded.artifact.postings.size() == built.artifact.postings.size());
  const auto rows = a::RowsFromArtifact(loaded.artifact);
  Check(rows.size() == 1 && rows[0].document_uuid == row.document_uuid && rows[0].version_uuid == row.version_uuid);
  auto corrupted = bytes; corrupted.back() ^= 1;
  Check(!a::ParseArtifactBinary(corrupted, open).ok);
  Check(!a::ParseArtifactBinary(bytes.substr(0, bytes.size() - 1), open).ok);
  Check(!a::ParseArtifactBinary("SBDOCPATH\nVERSION\t1\nEND\n", open).ok);
  // A fresh lookup key reloads the persisted native binding instead of deriving UUIDs.
  const auto reopened = a::DocumentPathProviderIdentityForContext(context, 1, identity.index_uuid);
  Check(reopened.index_uuid == identity.index_uuid && reopened.segment_uuid == identity.segment_uuid);
  auto other = context; other.database_uuid.bytes[15] ^= 1;
  Check(a::DocumentPathProviderIdentityForContext(other, 1).database_uuid.is_nil());
  context.database_uuid = {};
  Check(a::DocumentPathProviderIdentityForContext(context, 1).database_uuid.is_nil());
  std::filesystem::remove(request.artifact_path);
  std::filesystem::remove(directory);
}
