// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_header.hpp"
#include "page_skeleton.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using scratchbird::core::platform::Uuid;
using scratchbird::storage::disk::ClassifyPageHeader;
using scratchbird::storage::disk::PageHeader;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::SerializePageHeader;
using scratchbird::storage::page::BuiltinPageFamilyRegistry;
using scratchbird::storage::page::ClassifyPageBodyProductionAdmission;
using scratchbird::storage::page::PageBodyProductionAdmissionKind;
using scratchbird::storage::page::PageBodyProductionAdmissionKindName;

Uuid MakeV7Uuid(std::uint8_t seed) {
  Uuid uuid;
  for (std::size_t index = 0; index < uuid.bytes.size(); ++index) {
    uuid.bytes[index] =
        static_cast<scratchbird::core::platform::byte>(seed + index + 1);
  }
  uuid.bytes[6] = static_cast<scratchbird::core::platform::byte>(
      (uuid.bytes[6] & 0x0fu) | 0x70u);
  uuid.bytes[8] = static_cast<scratchbird::core::platform::byte>(
      (uuid.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

PageHeader HeaderFor(PageType page_type, std::uint8_t seed) {
  PageHeader header;
  header.page_size = 16384;
  header.page_type = page_type;
  header.database_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 1));
  header.filespace_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 32));
  header.page_uuid = MakeV7Uuid(static_cast<std::uint8_t>(seed + 64));
  header.page_number = static_cast<std::uint64_t>(seed) + 1;
  header.page_generation = 1;
  return header;
}

bool Expect(bool condition, const std::string& message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

bool ExpectAdmission(PageType page_type,
                     PageBodyProductionAdmissionKind expected_kind,
                     bool admitted,
                     bool mutating,
                     std::uint8_t seed) {
  const auto serialized = SerializePageHeader(HeaderFor(page_type, seed));
  if (!serialized.ok()) {
    std::cerr << PageTypeName(page_type)
              << " serialization failed: "
              << serialized.diagnostic.diagnostic_code << '\n';
    return false;
  }

  const auto classified = ClassifyPageHeader(serialized.serialized);
  const auto admission = ClassifyPageBodyProductionAdmission(classified);
  bool ok = true;
  ok = Expect(admission.kind == expected_kind,
              std::string(PageTypeName(page_type)) + " admission kind was " +
                  PageBodyProductionAdmissionKindName(admission.kind) +
                  " expected " +
                  PageBodyProductionAdmissionKindName(expected_kind)) &&
       ok;
  ok = Expect(admission.admitted == admitted,
              std::string(PageTypeName(page_type)) +
                  " admission boolean mismatch") &&
       ok;
  ok = Expect(admission.may_mutate_body == mutating,
              std::string(PageTypeName(page_type)) +
                  " mutation admission mismatch") &&
       ok;
  if (!admitted) {
    ok = Expect(!admission.diagnostic.diagnostic_code.empty(),
                std::string(PageTypeName(page_type)) +
                    " refusal did not expose a stable diagnostic") &&
         ok;
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;

  std::uint8_t seed = 1;
  for (const auto& descriptor : BuiltinPageFamilyRegistry()) {
    if (descriptor.cluster_only) {
      ok = ExpectAdmission(
               descriptor.page_type,
               PageBodyProductionAdmissionKind::external_cluster_provider_required,
               false,
               false,
               seed++) &&
           ok;
      continue;
    }
    if (descriptor.encrypted_or_opaque) {
      ok = ExpectAdmission(descriptor.page_type,
                           PageBodyProductionAdmissionKind::decryption_required,
                           false,
                           false,
                           seed++) &&
           ok;
      continue;
    }
    if (descriptor.reserved) {
      ok = ExpectAdmission(descriptor.page_type,
                           PageBodyProductionAdmissionKind::reserved_nonmutating,
                           false,
                           false,
                           seed++) &&
           ok;
      continue;
    }
    if (descriptor.supported_local_write) {
      ok = ExpectAdmission(
               descriptor.page_type,
               PageBodyProductionAdmissionKind::local_engine_mutating,
               true,
               true,
               seed++) &&
           ok;
      continue;
    }
    if (descriptor.supported_local_read) {
      ok = ExpectAdmission(
               descriptor.page_type,
               PageBodyProductionAdmissionKind::local_engine_read_only,
               true,
               false,
               seed++) &&
           ok;
      continue;
    }
    ok = ExpectAdmission(
             descriptor.page_type,
             PageBodyProductionAdmissionKind::body_refused,
             false,
             false,
             seed++) &&
         ok;
  }

  ok = ExpectAdmission(PageType::reserved_local,
                       PageBodyProductionAdmissionKind::reserved_nonmutating,
                       false,
                       false,
                       80) &&
       ok;
  ok = ExpectAdmission(
           PageType::cluster_transaction,
           PageBodyProductionAdmissionKind::external_cluster_provider_required,
           false,
           false,
           81) &&
       ok;
  ok = ExpectAdmission(PageType::encrypted_opaque,
                       PageBodyProductionAdmissionKind::decryption_required,
                       false,
                       false,
                       82) &&
       ok;

  if (!ok) {
    return 1;
  }
  std::cout << "public_page_body_production_refusal_gate=passed\n";
  return 0;
}
