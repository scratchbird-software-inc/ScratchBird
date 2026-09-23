// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/storage/database/native_drop_evidence.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
using namespace scratchbird::storage::database;
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
  NativeDropEvidence value{scratchbird::tests::FixtureUuid(1259,1),
      scratchbird::tests::FixtureUuid(1259,2), scratchbird::tests::FixtureUuid(1259,3),
      scratchbird::tests::FixtureUuid(1259,4), 0xfedcba9876543210ULL, 1};
  value.operation_uuid.bytes[9] = 0; value.operation_uuid.bytes[10] = '\n';
  value.operation_uuid.bytes[11] = '|'; value.actor_uuid.bytes[15] = 255;
  for (std::uint8_t mode=1; mode<=3; ++mode) {
    value.mode=mode;
    auto bytes=EncodeNativeDropEvidence(value);
    Check(DecodeNativeDropEvidence(bytes)==value);
    Check(std::equal(value.operation_uuid.bytes.begin(),value.operation_uuid.bytes.end(),bytes.begin()+40));
    Check(bytes[72]==0x10 && bytes[79]==0xfe);
    for (std::size_t n=0;n<bytes.size();++n)
      Check(!DecodeNativeDropEvidence(std::span(bytes).first(n)));
    bytes[0]='X';Check(!DecodeNativeDropEvidence(bytes));
    bytes=EncodeNativeDropEvidence(value);bytes[80]=0;Check(!DecodeNativeDropEvidence(bytes));
    bytes=EncodeNativeDropEvidence(value);
    std::array<std::uint8_t,82> extra{};std::copy(bytes.begin(),bytes.end(),extra.begin());
    Check(!DecodeNativeDropEvidence(extra));
  }
  value.local_transaction_id=0;Check(!DecodeNativeDropEvidence(EncodeNativeDropEvidence(value)));
  std::cout << "PASS binary lifecycle drop evidence\n";
}
