// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/sblr_runtime_logging.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace s = scratchbird::engine::sblr;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  s::SblrRuntimeLogRecord record;
  record.database_uuid = scratchbird::tests::FixtureUuid(1152, 1);
  record.database_uuid.bytes[10] = 0; record.database_uuid.bytes[15] = 255;
  record.statement_uuid = scratchbird::tests::FixtureUuid(1152, 2);
  record.local_transaction_id = 42;
  record.timestamp = "2026-09-22T12:00:00Z";
  record.message = std::string("a\0b\tc\nd", 7);
  const auto bytes = s::SerializeSblrRuntimeLogRecord(record);
  Check(!bytes.empty());
  Check(bytes.find(std::string(reinterpret_cast<const char*>(record.database_uuid.bytes.data()), 16)) != std::string::npos);
  s::SblrRuntimeLogRecord decoded;
  Check(s::DeserializeSblrRuntimeLogRecord(bytes, &decoded));
  Check(decoded.database_uuid == record.database_uuid && decoded.statement_uuid == record.statement_uuid &&
        decoded.cluster_uuid.is_nil() && decoded.local_transaction_id == 42 && decoded.message == record.message);
  Check(!s::DeserializeSblrRuntimeLogRecord(bytes.substr(0, bytes.size() - 1), &decoded));
  Check(!s::DeserializeSblrRuntimeLogRecord("SBLRLOG1\tdatabase_uuid=019f0000-0000-7000-8000-000000000001", &decoded));
  record.database_uuid.bytes[6] = 0x40;
  Check(s::SerializeSblrRuntimeLogRecord(record).empty());
}
