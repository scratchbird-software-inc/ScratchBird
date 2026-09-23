// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/transaction/local_commit_publication.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace api = scratchbird::engine::internal_api;
namespace codec = api::local_publication_codec;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  api::EngineRequestContext context;
  context.database_uuid = scratchbird::tests::FixtureUuid(1164, 1);
  context.transaction_uuid = scratchbird::tests::FixtureUuid(1164, 2);
  context.local_transaction_id = 42;
  auto object = scratchbird::tests::FixtureUuid(1164, 3);
  object.bytes[10] = 0; object.bytes[15] = 255;
  auto row = scratchbird::tests::FixtureUuid(1164, 4);
  auto version = scratchbird::tests::FixtureUuid(1164, 5);
  auto mutation = api::Mutation("row", "insert", object, row,
      std::string("a\0b", 3), 1, 2, "before", "after", context, version);
  auto different = api::Mutation("row", "insert", object, row,
      std::string("a\0b", 3), 1, 2, "before", "after", context,
      scratchbird::tests::FixtureUuid(1164, 6));
  Check(mutation.mutation_identity != different.mutation_identity);
  Check(mutation.idempotency_key != different.idempotency_key);
  api::LocalCommitPublicationArtifact artifact{"rows", "database_file", 100, codec::Digest("artifact")};
  std::string bytes;
  Check(codec::Encode(context, {mutation}, {artifact}, &bytes));
  Check(bytes.find(std::string(reinterpret_cast<const char*>(object.bytes.data()), 16)) != std::string::npos);
  api::LocalCommitPublicationRecoveryResult decoded;
  Check(codec::Decode(bytes, context, &decoded));
  Check(!decoded.ok); // A valid manifest never establishes transaction finality.
  Check(decoded.mutations.size() == 1 && decoded.artifacts.size() == 1);
  Check(decoded.mutations[0].object_identity == object && decoded.mutations[0].record_identity == row &&
      decoded.mutations[0].version_identity == version && decoded.mutations[0].physical_identity == std::string("a\0b", 3));
  auto wrong = context; wrong.database_uuid = row;
  Check(!codec::Decode(bytes, wrong, &decoded));
  wrong = context; wrong.transaction_uuid = row;
  Check(!codec::Decode(bytes, wrong, &decoded));
  wrong = context; ++wrong.local_transaction_id;
  Check(!codec::Decode(bytes, wrong, &decoded));
  for (std::size_t n=0; n<bytes.size(); ++n) Check(!codec::Decode(std::string_view(bytes).substr(0,n), context, &decoded));
  auto changed = bytes; changed[changed.size()/2] ^= 1;
  Check(!codec::Decode(changed, context, &decoded));
  Check(!codec::Decode("SBMGAP01\tdatabase_uuid=019f0000-0000-7000-8000-000000000001", context, &decoded));
  auto fields = codec::Fields(mutation);
  fields.text["generation_before"] = "01";
  Check(!codec::Decode(fields, &different));
  fields = codec::Fields(mutation); fields.text["object_identity"] = "legacy";
  Check(!codec::Decode(fields, &different));
  fields = codec::Fields(mutation); fields.identities.erase("record_identity");
  Check(!codec::Decode(fields, &different));
  mutation.object_identity = {};
  Check(!codec::Encode(context, {mutation}, {artifact}, &bytes));
}
