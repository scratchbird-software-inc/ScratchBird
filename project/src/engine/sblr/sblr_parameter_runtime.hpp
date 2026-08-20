// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
struct SblrParameterNodeV1 {
  std::uint64_t node_id=0; std::uint32_t parent_operand_ordinal=0,slot_ordinal=0;
  std::array<std::uint8_t,16> parameter_set_descriptor_uuid{};
  std::uint64_t parameter_set_generation=0;
  std::array<std::uint8_t,16> datatype_descriptor_uuid{};
  std::uint64_t datatype_descriptor_generation=0;
};
struct SblrParameterNodeTableV1 {std::vector<SblrParameterNodeV1> nodes;};
struct SblrParameterNodeTableCodecResultV1 {bool ok=false;std::string diagnostic_id="SBLR.OPERAND_INVALID",detail;SblrParameterNodeTableV1 table;std::vector<std::uint8_t> canonical_bytes;};
std::vector<std::uint8_t> EncodeSblrParameterNodeTableV1(const SblrParameterNodeTableV1&);
SblrParameterNodeTableCodecResultV1 DecodeSblrParameterNodeTableV1(const std::uint8_t*,std::size_t);
struct SblrParameterNodeReferenceV1 {std::uint32_t occurrence_ordinal=0;std::uint64_t node_id=0;std::array<std::uint8_t,32> table_sha256{};std::array<std::uint8_t,16> parameter_set_descriptor_uuid{};std::uint64_t parameter_set_generation=0;std::uint32_t slot_ordinal=0;};
std::vector<std::uint8_t> EncodeSblrParameterNodeReferenceV1(const SblrParameterNodeReferenceV1&);
bool DecodeSblrParameterNodeReferenceV1(const std::uint8_t*,std::size_t,SblrParameterNodeReferenceV1*);
bool ValidateSblrParameterReferenceBijectionV1(const SblrParameterNodeTableCodecResultV1&,const std::vector<SblrParameterNodeReferenceV1>&);

enum class SblrParameterDirectionV1:std::uint8_t {in=1,out=2,inout=3};
enum class SblrParameterValueStateV1:std::uint8_t {unbound=0,value=1,null_value=2};
struct SblrParameterValueRecordV1 {std::uint32_t slot_ordinal=0;std::array<std::uint8_t,16> slot_uuid{},datatype_descriptor_uuid{};std::uint64_t datatype_descriptor_generation=0;SblrParameterDirectionV1 direction=SblrParameterDirectionV1::in;SblrParameterValueStateV1 state=SblrParameterValueStateV1::unbound;std::vector<std::uint8_t> canonical_value_bytes;};
struct SblrParameterValueSetV1 {std::array<std::uint8_t,16> parameter_set_descriptor_uuid{};std::uint64_t descriptor_generation=0;std::array<std::uint8_t,16> execution_uuid{},statement_receipt_uuid{};std::vector<SblrParameterValueRecordV1> records;};
struct SblrParameterValueSetCodecResultV1 {bool ok=false;std::string diagnostic_id="SBLR.OPERAND_INVALID",detail;SblrParameterValueSetV1 value;std::vector<std::uint8_t> canonical_bytes;};
std::vector<std::uint8_t> EncodeSblrParameterValueSetV1(const SblrParameterValueSetV1&);
SblrParameterValueSetCodecResultV1 DecodeSblrParameterValueSetV1(const std::uint8_t*,std::size_t);

struct SblrParameterDemandV1 {std::uint64_t occurrence_id=0;std::uint32_t marker_ordinal=0;std::uint16_t context_code=0;std::uint8_t requested_direction=0,nullable_demand=0;};
struct SblrParameterNegotiateRequestV1 {std::array<std::uint8_t,16> preliminary_receipt_uuid{},mga_snapshot_uuid{};std::uint64_t catalog_generation=0,security_epoch=0,resource_epoch=0;std::vector<SblrParameterDemandV1> demands;std::array<std::uint8_t,32> demand_sha256{};};
struct SblrParameterMappingV1 {std::uint64_t occurrence_id=0;std::uint32_t slot_ordinal=0;std::array<std::uint8_t,16> slot_uuid{},datatype_descriptor_uuid{},datatype_type_uuid{};std::uint64_t datatype_descriptor_generation=0;std::uint8_t direction=0,nullable=0;};
struct SblrParameterNegotiateResultV1 {std::array<std::uint8_t,16> preliminary_receipt_uuid{},parameter_set_descriptor_uuid{},execution_uuid{};std::uint64_t descriptor_generation=0;std::vector<SblrParameterMappingV1> mappings;std::array<std::uint8_t,32> mapping_sha256{};};
struct SblrParameterFinalizeRequestV1 {std::array<std::uint8_t,16> preliminary_receipt_uuid{},parameter_set_descriptor_uuid{},execution_uuid{};std::uint64_t descriptor_generation=0;std::array<std::uint8_t,32> demand_sha256{},mapping_sha256{},sbpn_sha256{};std::vector<std::uint8_t> canonical_sbpn;};
struct SblrParameterAdmissionV1 {std::array<std::uint8_t,16> final_receipt_uuid{},admission_token_uuid{},parameter_set_descriptor_uuid{},execution_uuid{},prepared_uuid{},batch_uuid{},dynamic_uuid{};std::uint64_t descriptor_generation=0,prepared_generation=0,batch_generation=0,dynamic_generation=0;std::array<std::uint8_t,32> binding_sha256{};};
std::array<std::uint8_t,32> ComputeSblrParameterDemandSha256V1(const std::vector<SblrParameterDemandV1>&);
std::array<std::uint8_t,32> ComputeSblrParameterMappingSha256V1(const std::vector<SblrParameterMappingV1>&);
std::vector<std::uint8_t> EncodeSblrParameterNegotiateRequestV1(const SblrParameterNegotiateRequestV1&);
bool DecodeSblrParameterNegotiateRequestV1(const std::uint8_t*,std::size_t,SblrParameterNegotiateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrParameterNegotiateResultV1(const SblrParameterNegotiateResultV1&);
bool DecodeSblrParameterNegotiateResultV1(const std::uint8_t*,std::size_t,SblrParameterNegotiateResultV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrParameterFinalizeRequestV1(const SblrParameterFinalizeRequestV1&);
bool DecodeSblrParameterFinalizeRequestV1(const std::uint8_t*,std::size_t,SblrParameterFinalizeRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrParameterAdmissionV1(SblrParameterAdmissionV1*);
bool DecodeSblrParameterAdmissionV1(const std::uint8_t*,std::size_t,SblrParameterAdmissionV1*,std::string*);
enum class SblrParameterWirePrevalidationV1 : std::uint8_t {
  ok = 0,
  operand_invalid = 1,
  resource_exceeded = 2,
};
SblrParameterWirePrevalidationV1 PrevalidateSblrParameterNegotiateRequestV1(
    const std::uint8_t*, std::size_t);
SblrParameterWirePrevalidationV1 PrevalidateSblrParameterNegotiateResultV1(
    const std::uint8_t*, std::size_t);
SblrParameterWirePrevalidationV1 PrevalidateSblrParameterFinalizeRequestV1(
    const std::uint8_t*, std::size_t);

struct SblrPreparedParameterTemplateV1 {
  std::array<std::uint8_t,16> public_coordination_uuid{}, operation_uuid{},
      provisional_prepared_uuid{}, parameter_set_descriptor_uuid{},
      mga_snapshot_uuid{};
  std::uint64_t provisional_prepared_generation=0,
      descriptor_generation=0, executor_availability_generation=0,
      catalog_generation=0, security_epoch=0, resource_epoch=0;
  std::array<std::uint8_t,32> schema4015_sha256{}, sbpn_sha256{}, sbpa_sha256{},
      prepared_template_binding_sha256{};
  std::vector<std::uint8_t> canonical_schema4015, canonical_sbpa;
};
struct SblrPreparedParameterTemplateCodecResultV1 {
  bool ok=false;
  std::string diagnostic_id="SBLR.OPERAND_INVALID", detail;
  SblrPreparedParameterTemplateV1 value;
  std::vector<std::uint8_t> canonical_bytes;
};
std::vector<std::uint8_t> EncodeSblrPreparedParameterTemplateV1(
    SblrPreparedParameterTemplateV1*);
SblrPreparedParameterTemplateCodecResultV1
DecodeSblrPreparedParameterTemplateV1(const std::uint8_t*,std::size_t);
}
