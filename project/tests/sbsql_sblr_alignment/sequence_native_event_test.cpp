// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/lifecycle/sequence_generator_lifecycle.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>
namespace api = scratchbird::engine::internal_api;
void Check(bool value) { if (!value) std::abort(); }
int main() {
  api::EngineRequestContext context;
  context.database_path = (std::filesystem::temp_directory_path() /
      ("sb_sequence_native_" + std::to_string(getpid()))).string();
  auto identity = scratchbird::tests::FixtureUuid(1296, 1);
  identity.bytes[9] = 0; identity.bytes[10] = '\n'; identity.bytes[11] = '\t'; identity.bytes[12] = 255;
  api::BinaryCatalogMetadata metadata;
  metadata.text = {{"event_kind", "IDENTITY_BIND"}, {"creator_tx", "17"}, {"identity_value_kind", "row_uuid_identity"}};
  metadata.identities = {{"identity_binding_uuid", identity}, {"record_uuid", identity}, {"identity_value", identity}};
  std::string encoded;
  Check(api::EncodeBinaryCatalogMetadata(metadata, api::kSequenceGeneratorLifecycleEventMagic, &encoded));
  std::string framed;
  api::AppendBinaryU32(&framed, static_cast<std::uint32_t>(encoded.size()));
  framed += encoded;
  auto read = [&](const std::string& bytes, std::vector<api::RawSequenceEvent>* events) {
    std::ofstream stream(api::EventPath(context), std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), bytes.size()); stream.close(); Check(bool(stream));
    return api::ReadRawEvents(context, events);
  };
  std::vector<api::RawSequenceEvent> events;
  Check(read(framed + framed, &events).ok && events.size() == 2);
  Check(events[0].creator_tx == 17 && events[1].event_sequence == 2);
  const auto binding = api::BindingFromFields(events[0]);
  Check(binding.record_uuid == identity && binding.identity_value == api::EngineEvidenceValue{identity});
  Check(events[0].fields.text.count("record_uuid") == 0);
  for (std::size_t size = 1; size < framed.size(); ++size) {
    std::vector<api::RawSequenceEvent> unchanged(1);
    unchanged[0].event_sequence = 99;
    Check(!read(framed.substr(0, size), &unchanged).ok);
    Check(unchanged.size() == 1 && unchanged[0].event_sequence == 99);
  }
  Check(!read(framed + "x", &events).ok);
  Check(!read("SBSEQGEN1\tCREATE\t17\tgenerator_uuid=616263\n", &events).ok);
  auto invalid = framed; invalid[4] = 'X';
  Check(!read(invalid, &events).ok);
  metadata.text["record_uuid"] = "human-readable";
  Check(!api::EncodeBinaryCatalogMetadata(metadata, api::kSequenceGeneratorLifecycleEventMagic, &encoded));
  std::filesystem::remove(api::EventPath(context));
  Check(!api::IssueSequenceIdentity().is_nil());
  std::cout << "sequence native binary event regression PASS\n";
}
