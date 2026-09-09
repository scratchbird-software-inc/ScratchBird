#include "engine/sblr/sblr_security_create_privilege_template_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

sblr::PrivilegeTemplateUuid Uuid(std::uint8_t seed) {
  sblr::PrivilegeTemplateUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

sblr::PrivilegeTemplateSha Sha(std::uint8_t seed) {
  sblr::PrivilegeTemplateSha value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

}  // namespace

int main() {
  sblr::SblrSecurityCreatePrivilegeTemplateRequestV1 request;
  request.receipt = Uuid(1);
  request.occurrence = 7;
  request.template_occurrence = 3;
  request.object_kind = sblr::PrivilegeTemplateObjectKindV1::kTables;
  request.with_grant_option = true;
  request.template_name_utf8 = "future_readers";
  request.grantee_name_utf8 = "ROLE_SYSARCH";
  request.privilege_utf8 = "SELECT";
  auto request_bytes =
      sblr::EncodeSblrSecurityCreatePrivilegeTemplateRequestV1(request);
  Require(request_bytes.size() ==
              sblr::kSblrSecurityCreatePrivilegeTemplateRequestV1Bytes,
          "PTQX exact extent was not encoded");
  sblr::SblrSecurityCreatePrivilegeTemplateRequestV1 decoded_request;
  std::string detail;
  Require(sblr::DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(
              request_bytes.data(), request_bytes.size(), &decoded_request,
              &detail) &&
              decoded_request.template_name_utf8 == "future_readers" &&
              decoded_request.grantee_name_utf8 == "ROLE_SYSARCH" &&
              decoded_request.privilege_utf8 == "SELECT" &&
              decoded_request.with_grant_option,
          "PTQX strict round trip failed");
  auto request_tamper = request_bytes;
  request_tamper[704] ^= 1;
  Require(!sblr::DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(
              request_tamper.data(), request_tamper.size(), &decoded_request,
              &detail),
          "PTQX evidence tamper was accepted");
  request_tamper = request_bytes;
  request_tamper[736] = 1;
  Require(!sblr::DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(
              request_tamper.data(), request_tamper.size(), &decoded_request,
              &detail),
          "PTQX reserved-byte tamper was accepted");

  sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 descriptor;
  descriptor.receipt = request.receipt;
  descriptor.occurrence = request.occurrence;
  descriptor.template_occurrence = request.template_occurrence;
  descriptor.object_kind = request.object_kind;
  descriptor.with_grant_option = request.with_grant_option;
  descriptor.template_uuid = Uuid(21);
  descriptor.template_generation = 1;
  descriptor.owner_principal_uuid = Uuid(41);
  descriptor.grantee_uuid = Uuid(61);
  descriptor.owning_transaction_uuid = Uuid(81);
  descriptor.owning_local_transaction_id = 17;
  descriptor.statement_snapshot_uuid = Uuid(101);
  descriptor.catalog_epoch_uuid = Uuid(121);
  descriptor.catalog_generation = 5;
  descriptor.security_context_uuid = Uuid(141);
  descriptor.security_epoch = 6;
  descriptor.policy_snapshot_uuid = Uuid(161);
  descriptor.policy_generation = 7;
  descriptor.resource_grant_uuid = Uuid(181);
  descriptor.resource_generation = 8;
  descriptor.recovery_uuid = Uuid(201);
  descriptor.recovery_generation = 1;
  descriptor.syntax_demand_sha256 = Sha(3);
  descriptor.definition_sha256 = Sha(43);
  descriptor.idempotency_sha256 = Sha(83);
  descriptor.availability = 9;
  auto descriptor_bytes =
      sblr::EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
          descriptor, false);
  Require(descriptor_bytes.size() ==
              sblr::kSblrSecurityCreatePrivilegeTemplateDescriptorV1Bytes,
          "PTDD exact extent was not encoded");
  sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 decoded_descriptor;
  Require(sblr::DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
              descriptor_bytes.data(), descriptor_bytes.size(),
              &decoded_descriptor, &detail, false) &&
              decoded_descriptor.template_uuid == descriptor.template_uuid &&
              decoded_descriptor.grantee_uuid == descriptor.grantee_uuid &&
              decoded_descriptor.definition_sha256 ==
                  descriptor.definition_sha256,
          "PTDD strict round trip failed");
  auto operand_bytes = descriptor_bytes;
  std::copy_n("PTDO", 4, operand_bytes.begin());
  sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 decoded_operand;
  Require(sblr::DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
              operand_bytes.data(), operand_bytes.size(), &decoded_operand,
              &detail, true) &&
              std::equal(descriptor_bytes.begin() + 4, descriptor_bytes.end(),
                         operand_bytes.begin() + 4),
          "PTDD to PTDO was not a literal magic-only projection");
  auto descriptor_tamper = operand_bytes;
  descriptor_tamper[416] ^= 1;
  Require(!sblr::DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
              descriptor_tamper.data(), descriptor_tamper.size(),
              &decoded_operand, &detail, true),
          "PTDO evidence tamper was accepted");

  sblr::SblrSecurityCreatePrivilegeTemplateResultV1 result;
  result.receipt = descriptor.receipt;
  result.template_uuid = descriptor.template_uuid;
  result.template_generation = descriptor.template_generation;
  result.owner_principal_uuid = descriptor.owner_principal_uuid;
  result.grantee_uuid = descriptor.grantee_uuid;
  result.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id =
      descriptor.owning_local_transaction_id;
  result.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_generation = 10;
  result.resource_generation = descriptor.resource_generation;
  result.definition_sha256 = descriptor.definition_sha256;
  result.descriptor_evidence_sha256 = decoded_descriptor.evidence;
  result.mutation_uuid = Uuid(221);
  result.cache_invalidation_epoch = 10;
  result.template_created = true;
  result.availability = descriptor.availability;
  result.publication_barrier = Uuid(241);
  auto result_bytes =
      sblr::EncodeSblrSecurityCreatePrivilegeTemplateResultV1(result);
  Require(result_bytes.size() ==
              sblr::kSblrSecurityCreatePrivilegeTemplateResultV1Bytes,
          "PTRS exact extent was not encoded");
  sblr::SblrSecurityCreatePrivilegeTemplateResultV1 decoded_result;
  Require(sblr::DecodeSblrSecurityCreatePrivilegeTemplateResultV1(
              result_bytes.data(), result_bytes.size(), &decoded_result,
              &detail) &&
              decoded_result.template_created &&
              !decoded_result.exact_idempotent_replay &&
              decoded_result.template_uuid == descriptor.template_uuid &&
              decoded_result.descriptor_evidence_sha256 ==
                  decoded_descriptor.evidence,
          "PTRS strict round trip failed");
  auto result_tamper = result_bytes;
  result_tamper[256] ^= 1;
  Require(!sblr::DecodeSblrSecurityCreatePrivilegeTemplateResultV1(
              result_tamper.data(), result_tamper.size(), &decoded_result,
              &detail),
          "PTRS evidence tamper was accepted");
  return EXIT_SUCCESS;
}
