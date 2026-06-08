// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_body_integrity.hpp"

#include "allocation_map_page.hpp"
#include "hash_digest.hpp"
#include "index_btree_page.hpp"
#include "index_hash_page.hpp"
#include "index_specialized_pages.hpp"
#include "overflow_persistence.hpp"
#include "row_data_page.hpp"
#include "structured_page_body.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
namespace core_hash = scratchbird::core::hash;
using scratchbird::storage::disk::ClassifyPageHeader;
using scratchbird::storage::disk::PageClassification;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::ParsePageHeader;

inline constexpr std::array<byte, 8> kRowMagicV2 = {
    'S', 'B', 'R', 'O', 'W', '0', '0', '2'};
inline constexpr std::array<byte, 8> kRowMagicV1 = {
    'S', 'B', 'R', 'O', 'W', '0', '0', '1'};
inline constexpr std::array<byte, 8> kBtreeMagic = {
    'S', 'B', 'I', 'D', 'X', '0', '0', '1'};
inline constexpr std::array<byte, 8> kHashMagic = {
    'S', 'B', 'I', 'H', '0', '0', '0', '1'};
inline constexpr std::array<byte, 8> kOverflowMagic = {
    'S', 'B', 'O', 'V', 'C', 'H', '0', '1'};
inline constexpr std::array<byte, 8> kStructuredMagic = {
    'S', 'B', 'S', 'T', 'R', '0', '0', '1'};

Status IntegrityOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status IntegrityErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

PageBodyChecksumResult ChecksumError(std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  PageBodyChecksumResult result;
  result.status = IntegrityErrorStatus();
  result.diagnostic = MakePageBodyIntegrityDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      std::move(message_key),
                                                      std::move(detail));
  return result;
}

PageBodyAgreementResult AgreementError(PageBodyAgreementKind kind,
                                       std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail = {}) {
  PageBodyAgreementResult result;
  result.status = IntegrityErrorStatus();
  result.kind = kind;
  result.diagnostic = MakePageBodyIntegrityDiagnostic(result.status,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(detail));
  return result;
}

template <std::size_t N>
bool StartsWith(const std::vector<byte>& body, const std::array<byte, N>& magic) {
  return body.size() >= magic.size() &&
         std::equal(magic.begin(), magic.end(), body.begin());
}

u64 Fnv1a64(const std::vector<byte>& body, u64 seed) {
  u64 hash = seed;
  for (const byte value : body) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<byte> DigestMaterialFromU64(u64 low64) {
  std::vector<byte> material(sizeof(low64), 0);
  scratchbird::core::platform::StoreLittle64(material.data(), low64);
  return material;
}

void AttachCryptographicDigest(PageBodyChecksumDigest* digest,
                               const core_hash::HashDigestResult& computed,
                               std::string algorithm) {
  digest->digest_algorithm = std::move(algorithm);
  digest->digest_material = core_hash::DigestVector(computed.digest);
  digest->low64 = core_hash::DigestLow64(computed.digest);
  digest->high64 = core_hash::DigestHigh64(computed.digest);
  digest->digest_bytes = computed.digest_bytes;
}

PageBodyChecksumResult HashFailure(const core_hash::HashDigestResult& failure) {
  PageBodyChecksumResult result;
  result.status = failure.status;
  result.diagnostic = MakePageBodyIntegrityDiagnostic(
      result.status,
      failure.diagnostic.diagnostic_code.empty()
          ? "SB-PAGE-CHECKSUM-CRYPTO-DIGEST-FAILED"
          : failure.diagnostic.diagnostic_code,
      failure.diagnostic.message_key.empty()
          ? "storage.page_body_integrity.crypto_digest_failed"
          : failure.diagnostic.message_key);
  return result;
}

PageBodyKind DetectMagicBodyKind(const std::vector<byte>& body) {
  if (StartsWith(body, kRowMagicV2) || StartsWith(body, kRowMagicV1)) {
    return PageBodyKind::row_data;
  }
  if (StartsWith(body, kBtreeMagic)) {
    return PageBodyKind::index_btree;
  }
  if (StartsWith(body, kHashMagic)) {
    return PageBodyKind::index_hash;
  }
  if (StartsWith(body, kOverflowMagic)) {
    return PageBodyKind::blob_overflow;
  }
  if (StartsWith(body, kStructuredMagic)) {
    return PageBodyKind::structured;
  }
  if (StartsWith(body, kAllocationMapPageBodyMagic)) {
    return PageBodyKind::allocation_map;
  }
  return PageBodyKind::unknown;
}

bool IsBtreePageType(PageType page_type) {
  return page_type == PageType::index_btree ||
         page_type == PageType::index_btree_root ||
         page_type == PageType::index_btree_branch ||
         page_type == PageType::index_btree_leaf ||
         page_type == PageType::index_btree_posting;
}

bool IsSpecializedIndexPageType(PageType page_type) {
  const IndexPageFamilyKind family = IndexPageFamilyKindForPageType(page_type);
  return family != IndexPageFamilyKind::unknown &&
         family != IndexPageFamilyKind::btree &&
         family != IndexPageFamilyKind::hash;
}

bool BtreeKindMatchesPageType(PageType page_type, IndexBtreePageKind kind) {
  if (page_type == PageType::index_btree) {
    return true;
  }
  switch (kind) {
    case IndexBtreePageKind::root:
      return page_type == PageType::index_btree_root;
    case IndexBtreePageKind::internal:
      return page_type == PageType::index_btree_branch;
    case IndexBtreePageKind::leaf:
      return page_type == PageType::index_btree_leaf;
    case IndexBtreePageKind::unknown:
      return false;
  }
  return false;
}

void AddCommonEvidence(PageBodyAgreementResult* result) {
  result->evidence.push_back("page_type=" +
                             std::string(PageTypeName(result->header.page_type)));
  result->evidence.push_back("page_family=" +
                             std::string(PageFamilyName(result->page_family)));
  result->evidence.push_back("body_kind=" +
                             std::string(PageBodyKindName(result->body_kind)));
  result->evidence.push_back("checksum_profile=" +
                             std::string(PageBodyChecksumProfileName(
                                 result->checksum_digest.profile)));
  result->evidence.push_back("checksum_digest_algorithm=" +
                             result->checksum_digest.digest_algorithm);
  result->evidence.push_back("checksum_digest_bytes=" +
                             std::to_string(result->checksum_digest.digest_bytes));
  result->evidence.push_back(std::string("protected_material_supplied=") +
                             (result->checksum_digest.protected_material_supplied
                                  ? "true"
                                  : "false"));
  result->evidence.push_back(std::string("production_admitted=") +
                             (result->production_admitted ? "true" : "false"));
  result->evidence.push_back(std::string("production_mutating=") +
                             (result->production_mutating ? "true" : "false"));
  result->evidence.push_back("body_family_agreement_valid=" +
                             std::string(result->body_family_agreement_valid
                                             ? "true"
                                             : "false"));
}

void AttachHeaderAndProduction(PageBodyAgreementResult* result,
                               const PageHeader& header,
                               const PageClassification& classification) {
  result->header = header;
  const auto family = LookupPageFamily(header.page_type);
  if (family.ok()) {
    result->page_family = family.descriptor.family;
  }
  const auto admission = ClassifyPageBodyProductionAdmission(classification);
  result->production_admitted = admission.admitted;
  result->production_mutating =
      admission.kind == PageBodyProductionAdmissionKind::local_engine_mutating;
}

PageBodyAgreementResult FinalAgreement(PageBodyAgreementResult result) {
  result.status = IntegrityOkStatus();
  result.kind = PageBodyAgreementKind::accepted;
  result.body_family_agreement_valid = true;
  AddCommonEvidence(&result);
  return result;
}

PageBodyAgreementResult BodyParseRefusal(const DiagnosticRecord& diagnostic,
                                         const PageHeader& header,
                                         const PageBodyChecksumDigest& digest,
                                         const PageClassification& classification,
                                         PageBodyKind body_kind) {
  PageBodyAgreementResult result =
      AgreementError(PageBodyAgreementKind::body_parse_refused,
                     "SB-PAGE-BODY-AGREEMENT-PARSE-REFUSED",
                     "storage.page_body_integrity.parse_refused",
                     diagnostic.diagnostic_code);
  AttachHeaderAndProduction(&result, header, classification);
  result.body_kind = body_kind;
  result.checksum_digest = digest;
  AddCommonEvidence(&result);
  return result;
}

PageBodyAgreementResult FamilyMismatch(const PageHeader& header,
                                       const PageBodyChecksumDigest& digest,
                                       const PageClassification& classification,
                                       PageBodyKind body_kind,
                                       std::string detail) {
  PageBodyAgreementResult result =
      AgreementError(PageBodyAgreementKind::body_family_mismatch,
                     "SB-PAGE-BODY-AGREEMENT-FAMILY-MISMATCH",
                     "storage.page_body_integrity.family_mismatch",
                     std::move(detail));
  AttachHeaderAndProduction(&result, header, classification);
  result.body_kind = body_kind;
  result.checksum_digest = digest;
  AddCommonEvidence(&result);
  return result;
}

}  // namespace

const char* PageBodyChecksumProfileName(PageBodyChecksumProfile profile) {
  switch (profile) {
    case PageBodyChecksumProfile::fast: return "fast";
    case PageBodyChecksumProfile::strong: return "strong";
    case PageBodyChecksumProfile::protected_keyed: return "protected_keyed";
    case PageBodyChecksumProfile::unknown: return "unknown";
  }
  return "unknown";
}

const char* PageBodyKindName(PageBodyKind kind) {
  switch (kind) {
    case PageBodyKind::row_data: return "row_data";
    case PageBodyKind::index_btree: return "index_btree";
    case PageBodyKind::index_hash: return "index_hash";
    case PageBodyKind::index_specialized: return "index_specialized";
    case PageBodyKind::blob_overflow: return "blob_overflow";
    case PageBodyKind::structured: return "structured";
    case PageBodyKind::allocation_map: return "allocation_map";
    case PageBodyKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* PageBodyAgreementKindName(PageBodyAgreementKind kind) {
  switch (kind) {
    case PageBodyAgreementKind::accepted: return "accepted";
    case PageBodyAgreementKind::header_refused: return "header_refused";
    case PageBodyAgreementKind::checksum_refused: return "checksum_refused";
    case PageBodyAgreementKind::body_parse_refused: return "body_parse_refused";
    case PageBodyAgreementKind::body_family_mismatch: return "body_family_mismatch";
    case PageBodyAgreementKind::page_kind_mismatch: return "page_kind_mismatch";
    case PageBodyAgreementKind::unsupported_body_kind: return "unsupported_body_kind";
  }
  return "unsupported_body_kind";
}

PageBodyChecksumResult ComputePageBodyChecksumDigest(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    const std::vector<byte>& protected_key_material) {
  PageBodyChecksumResult result;
  result.status = IntegrityOkStatus();
  result.digest.profile = profile;

  switch (profile) {
    case PageBodyChecksumProfile::fast:
      result.digest.low64 = Fnv1a64(body, 1469598103934665603ull);
      result.digest.high64 = 0;
      result.digest.digest_bytes = 8;
      result.digest.digest_algorithm = "fnv1a64-diagnostic";
      result.digest.digest_material = DigestMaterialFromU64(result.digest.low64);
      result.matched = true;
      return result;
    case PageBodyChecksumProfile::strong:
    {
      const auto computed = core_hash::ComputeSha256Digest(body);
      if (!computed.ok()) {
        return HashFailure(computed);
      }
      AttachCryptographicDigest(&result.digest, computed, "sha256");
      result.matched = true;
      return result;
    }
    case PageBodyChecksumProfile::protected_keyed:
      result.digest.protected_material_required = true;
      result.digest.protected_material_supplied =
          !protected_key_material.empty();
      if (protected_key_material.empty()) {
        return ChecksumError("SB-PAGE-CHECKSUM-PROTECTED-MATERIAL-REQUIRED",
                             "storage.page_body_integrity.protected_material_required");
      }
    {
      const auto computed =
          core_hash::ComputeHmacSha256Digest(protected_key_material, body);
      if (!computed.ok()) {
        return HashFailure(computed);
      }
      AttachCryptographicDigest(&result.digest, computed, "hmac-sha256");
      result.digest.protected_material_required = true;
      result.digest.protected_material_supplied = true;
      result.matched = true;
      return result;
    }
    case PageBodyChecksumProfile::unknown:
      break;
  }

  return ChecksumError("SB-PAGE-CHECKSUM-PROFILE-UNKNOWN",
                       "storage.page_body_integrity.checksum_profile_unknown",
                       std::to_string(static_cast<u16>(profile)));
}

PageBodyChecksumResult VerifyPageBodyChecksumDigest(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    u64 expected_low64,
    u64 expected_high64,
    const std::vector<byte>& protected_key_material) {
  PageBodyChecksumResult result =
      ComputePageBodyChecksumDigest(profile, body, protected_key_material);
  if (!result.ok()) {
    return result;
  }
  result.matched = result.digest.low64 == expected_low64 &&
                   result.digest.high64 == expected_high64;
  if (!result.matched) {
    result.status = IntegrityErrorStatus();
    result.diagnostic =
        MakePageBodyIntegrityDiagnostic(result.status,
                                        "SB-PAGE-CHECKSUM-MISMATCH",
                                        "storage.page_body_integrity.checksum_mismatch",
                                        PageBodyChecksumProfileName(profile));
  }
  return result;
}

PageBodyChecksumResult VerifyPageBodyChecksumDigestMaterial(
    PageBodyChecksumProfile profile,
    const std::vector<byte>& body,
    const std::vector<byte>& expected_digest_material,
    const std::vector<byte>& protected_key_material) {
  PageBodyChecksumResult result =
      ComputePageBodyChecksumDigest(profile, body, protected_key_material);
  if (!result.ok()) {
    return result;
  }
  result.matched =
      core_hash::ConstantTimeEqual(result.digest.digest_material,
                                   expected_digest_material);
  if (!result.matched) {
    result.status = IntegrityErrorStatus();
    result.diagnostic =
        MakePageBodyIntegrityDiagnostic(result.status,
                                        "SB-PAGE-CHECKSUM-MISMATCH",
                                        "storage.page_body_integrity.checksum_mismatch",
                                        PageBodyChecksumProfileName(profile));
  }
  return result;
}

PageBodyAgreementResult ValidatePageBodyAgreement(
    const PageBodyAgreementRequest& request) {
  const auto checksum =
      ComputePageBodyChecksumDigest(request.checksum_profile,
                                    request.body,
                                    request.protected_key_material);
  if (!checksum.ok()) {
    PageBodyAgreementResult result =
        AgreementError(PageBodyAgreementKind::checksum_refused,
                       checksum.diagnostic.diagnostic_code,
                       checksum.diagnostic.message_key);
    result.checksum_digest = checksum.digest;
    AddCommonEvidence(&result);
    return result;
  }

  const auto parsed_header = ParsePageHeader(request.header);
  if (!parsed_header.ok()) {
    PageBodyAgreementResult result =
        AgreementError(PageBodyAgreementKind::header_refused,
                       "SB-PAGE-BODY-AGREEMENT-HEADER-REFUSED",
                       "storage.page_body_integrity.header_refused",
                       parsed_header.diagnostic.diagnostic_code);
    result.checksum_digest = checksum.digest;
    AddCommonEvidence(&result);
    return result;
  }

  const PageClassification classification = ClassifyPageHeader(request.header);
  PageBodyAgreementResult base;
  AttachHeaderAndProduction(&base, parsed_header.header, classification);
  base.checksum_digest = checksum.digest;

  PageBodyKind body_kind = DetectMagicBodyKind(request.body);
  if (body_kind == PageBodyKind::unknown &&
      IsSpecializedIndexPageType(parsed_header.header.page_type)) {
    body_kind = PageBodyKind::index_specialized;
  }

  base.body_kind = body_kind;

  switch (body_kind) {
    case PageBodyKind::row_data: {
      const auto parsed =
          ParseRowDataPageBody(request.body, parsed_header.header.page_number);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      if (parsed_header.header.page_type != PageType::row_data) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed_header.header.page_type));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::index_btree: {
      const auto parsed =
          ParseIndexBtreePageBody(request.body, parsed_header.header.page_number);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      base.index_family = IndexPageFamilyKind::btree;
      if (!IsBtreePageType(parsed_header.header.page_type)) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed_header.header.page_type));
      }
      if (!BtreeKindMatchesPageType(parsed_header.header.page_type,
                                    parsed.body.page_kind)) {
        PageBodyAgreementResult result =
            AgreementError(PageBodyAgreementKind::page_kind_mismatch,
                           "SB-PAGE-BODY-AGREEMENT-PAGE-KIND-MISMATCH",
                           "storage.page_body_integrity.page_kind_mismatch",
                           IndexBtreePageKindName(parsed.body.page_kind));
        AttachHeaderAndProduction(&result, parsed_header.header, classification);
        result.body_kind = body_kind;
        result.index_family = IndexPageFamilyKind::btree;
        result.checksum_digest = checksum.digest;
        AddCommonEvidence(&result);
        return result;
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::index_hash: {
      const auto parsed =
          ParseIndexHashPageBody(request.body, parsed_header.header.page_number);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      base.index_family = IndexPageFamilyKind::hash;
      if (parsed_header.header.page_type != PageType::index_hash) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed_header.header.page_type));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::index_specialized: {
      const auto parsed = ParseIndexSpecializedPageBody(request.body);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      const IndexPageFamilyKind expected =
          IndexPageFamilyKindForPageType(parsed_header.header.page_type);
      base.index_family = parsed.body.header.family;
      if (expected == IndexPageFamilyKind::unknown ||
          parsed.body.header.family != expected ||
          parsed.body.header.page_type != parsed_header.header.page_type) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              IndexPageFamilyKindName(parsed.body.header.family));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::blob_overflow: {
      const auto parsed = ParseOverflowChunkPageBody(request.body);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      if (parsed_header.header.page_type != PageType::blob) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed_header.header.page_type));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::structured: {
      const auto parsed = ParseStructuredPageBody(request.body);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      if (parsed.body.page_type != parsed_header.header.page_type) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed.body.page_type));
      }
      const auto family = LookupPageFamily(parsed_header.header.page_type);
      if (!family.ok() || parsed.body.page_family != family.descriptor.family) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageFamilyName(parsed.body.page_family));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::allocation_map: {
      const auto parsed = ParseAllocationMapPageBody(request.body);
      if (!parsed.ok()) {
        return BodyParseRefusal(parsed.diagnostic,
                                parsed_header.header,
                                checksum.digest,
                                classification,
                                body_kind);
      }
      if (parsed_header.header.page_type != PageType::allocation_map ||
          parsed.body.allocation_map_page_number !=
              parsed_header.header.page_number) {
        return FamilyMismatch(parsed_header.header,
                              checksum.digest,
                              classification,
                              body_kind,
                              PageTypeName(parsed_header.header.page_type));
      }
      return FinalAgreement(base);
    }
    case PageBodyKind::unknown:
      break;
  }

  PageBodyAgreementResult result =
      AgreementError(PageBodyAgreementKind::unsupported_body_kind,
                     "SB-PAGE-BODY-AGREEMENT-UNSUPPORTED-BODY-KIND",
                     "storage.page_body_integrity.unsupported_body_kind",
                     PageTypeName(parsed_header.header.page_type));
  AttachHeaderAndProduction(&result, parsed_header.header, classification);
  result.body_kind = PageBodyKind::unknown;
  result.checksum_digest = checksum.digest;
  AddCommonEvidence(&result);
  return result;
}

DiagnosticRecord MakePageBodyIntegrityDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.body_integrity");
}

}  // namespace scratchbird::storage::page
