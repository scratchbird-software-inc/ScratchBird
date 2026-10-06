// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/core/datatypes/datatype_type_codec_identity_v3.hpp"
#include "../../src/core/datatypes/datatype_binary.hpp"
#include "../../src/core/datatypes/datatype_conformance_manifest.hpp"
#include "../../src/core/datatypes/datatype_exchange.hpp"
#include "../../src/core/datatypes/datatype_layout.hpp"
#include "../../src/core/datatypes/datatype_operations.hpp"
#include "../../src/core/datatypes/datatype_physical_encoding.hpp"
#include "../../src/core/datatypes/datatype_storage_identity.hpp"
#include "../../src/core/datatypes/datatype_wire_metadata.hpp"
#include "../../src/core/hash/hash_digest.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace allocation_probe {
thread_local bool fail_next = false;
}

void* operator new(std::size_t bytes) {
  if (allocation_probe::fail_next) {
    allocation_probe::fail_next = false;
    throw std::bad_alloc();
  }
  if (void* value = std::malloc(bytes == 0 ? 1 : bytes)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Check(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename T>
concept HasDescriptorPolicyMember = requires(T value) {
  value.descriptor_policy;
};

using Byte = scratchbird::core::platform::byte;

struct CorePolicyFixture {
  std::string_view uuid;
  std::uint64_t generation;
};

struct CoreD708RowFixture {
  std::string_view canonical_name;
  std::string_view descriptor_uuid;
  std::uint64_t descriptor_generation;
  std::string_view type_uuid;
  std::uint64_t type_generation;
  std::string_view codec_uuid;
  std::string_view codec_id;
  std::uint16_t codec_version;
  std::uint64_t codec_generation;
  std::uint32_t canonical_value_bytes;
  bool null_supported;
  std::uint32_t minimum_bytes;
  std::uint32_t maximum_bytes;
  std::uint32_t exact_or_transport_bytes;
  bool variable_width;
  bool exact_zero_is_width_marker;
  bool has_byte_order;
  std::string_view byte_order;
  bool has_signed;
  bool signed_value;
  std::string_view representation;
  std::string_view charset;
  bool shortest_form_utf8_required;
  bool descriptor_bound_collation_required;
  bool empty_value_distinct_from_sql_null;
  bool sql_null_requires_zero_payload;
  bool variable_width_storage_without_truncation;
  bool has_invalid_encoding_diagnostic;
  std::string_view invalid_encoding_diagnostic_id;
  std::string_view numeric_context_uuid;
  std::uint64_t numeric_context_generation;
  std::string_view special_value_policy_uuid;
  std::uint64_t special_value_policy_generation;
  std::string_view comparison_policy_uuid;
  std::uint64_t comparison_policy_generation;
  std::string_view comparison_profile;
  bool allow_special_values;
  bool has_canonical_type_code;
  std::uint32_t canonical_type_code;
  CorePolicyFixture descriptor_policy;
  CorePolicyFixture canonicalization_policy;
  CorePolicyFixture ordering_policy;
  CorePolicyFixture hash_policy;
  CorePolicyFixture operation_policy;
};


inline constexpr std::string_view kCoreD708RowsCanonicalJson =
R"CORE_D708_JSON([{"canonical_name":"boolean","canonical_value_bytes":{"byte_order":"single_byte","exact":1,"maximum":1,"minimum":1,"representation":"exact_00_FALSE_or_01_TRUE_all_other_bytes_invalid","signed":false},"codec_generation":1,"codec_id":"datatype.boolean.u8.v1","codec_uuid":"01a1010b-2e50-73c3-bdc8-ca82fc1fae5c","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"01000000-626f-7f6c-a561-6e0000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.boolean.v1","type_generation":1,"type_uuid":"01000000-626f-7f6c-a561-6e0000000000","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"int32","canonical_value_bytes":{"byte_order":"little_endian","exact":4,"maximum":4,"minimum":4,"representation":"twos_complement","signed":true},"codec_generation":1,"codec_id":"datatype.int32.le.v1","codec_uuid":"01a1010b-2e51-7c10-b90a-af9f08d9cd79","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d716","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.int32.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d717","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"bigint","canonical_value_bytes":{"byte_order":"little_endian","exact":8,"maximum":8,"minimum":8,"representation":"twos_complement","signed":true},"codec_generation":1,"codec_id":"datatype.int64.le.v1","codec_uuid":"01a1010b-2e52-79a4-8669-a9a7cb89bd21","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d711","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"unsupported_in_sblr_literal_v1","row_id":"datatype.bigint.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d712","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"decimal","canonical_value_bytes":{"byte_order":"little_endian","exact":24,"maximum":24,"maximum_precision":38,"maximum_scale":38,"minimum":24,"representation":"exact_decimal_header_and_five_base1e9_coefficient_groups","signed":true},"codec_generation":1,"codec_id":"datatype.decimal.base1e9.le.v1","codec_uuid":"01a1010b-2e54-777f-8089-a807f43084c2","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"a0000000-6465-7369-ad61-6c0000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"unsupported_in_sblr_literal_v1","row_id":"datatype.decimal.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d713","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"int128","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"twos_complement","signed":true},"codec_generation":1,"codec_id":"datatype.int128.le.v1","codec_uuid":"01a1010b-2e53-7fef-b6fa-1d9300cd10c8","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d714","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.int128.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d715","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"text","canonical_value_bytes":{"byte_order":"byte_sequence","exact":0,"exact_zero_semantics":"descriptor_width_marker_not_payload_length","maximum":16777216,"minimum":0,"representation":"exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization","signed":false,"width":"variable"},"codec_generation":1,"codec_id":"datatype.text.utf8.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d71a","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d718","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.text.v1","text_semantics":{"canonical_family":"text","charset":"UTF-8","charset_rule":"exact_well_formed_shortest_form_UTF8_Unicode_scalar_values_and_no_charset_inference_from_name_or_payload","collation":"exact_descriptor_bound_collation_UUID_generation_and_resource_epoch_required_for_equality_ordering_grouping_hashing_or_indexing_with_no_codec_name_byte_or_host_locale_fallback","empty_value":"admitted_value_present_with_zero_payload_bytes_and_distinct_from_SQL_NULL","length_bytes":"0_to_16777216_under_the_live_operation_and_resource_ceiling","length_chars":"descriptor_bound_or_unbounded_when_the_descriptor_field_is_null","length_units":"byte_length_and_Unicode_scalar_value_length_are_distinct","malformed_sequence":"reject_with_CTB.TEXT.INVALID_ENCODING","normalization":"exact_descriptor_bound_normalization_policy_and_resource_epoch_required_when_the_operation_requires_normalization_with_no_implicit_default","null_state":"SQL_NULL_only_in_the_containing_slot_null_state_with_zero_payload_bytes","operation_bound":"minimum_of_16777216_live_resource_grant_and_the_calling_operation_profile_ceiling","padding":"none_in_the_canonical_type_and_only_an_explicit_domain_or_compatibility_profile_may_add_padding","storage":"variable_width_inline_or_overflow_without_truncation"},"type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d719","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"real64","canonical_value_bytes":{"byte_order":"little_endian","exact":8,"maximum":8,"minimum":8,"representation":"IEEE754_binary64"},"codec_generation":1,"codec_id":"datatype.real64.ieee754.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d733","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d731","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.real64.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d732","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uuid","canonical_value_bytes":{"byte_order":"byte_sequence","exact":16,"maximum":16,"minimum":16,"representation":"sixteen_UUID_value_octets_without_text_conversion","value_uuid_versions":"all_128_bit_values_including_nil_system_identity_validation_remains_separate"},"codec_generation":1,"codec_id":"datatype.uuid.binary16.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d736","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d734","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uuid.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d735","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"geometry","canonical_value_bytes":{"byte_order":"byte_sequence","coordinates":"two_IEEE754_binary64_big_endian_values","maximum":24,"minimum":24,"negative_zero":"refuse","nonfinite":"refuse","other_geometry_encodings":"refuse_until_separately_admitted","prefix_hex":"5342503101020000","representation":"SBP1_finite_2D_point_big_endian_coordinates","transport_width":0},"codec_generation":1,"codec_id":"datatype.geometry.sbp1.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d739","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d737","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.geometry.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d738","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uint64","canonical_value_bytes":{"byte_order":"little_endian","exact":8,"maximum":8,"minimum":8,"representation":"unsigned_64_bit_integer","signed":false},"codec_generation":1,"codec_id":"datatype.uint64.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d73c","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d73a","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uint64.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d73b","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"json_document","canonical_value_bytes":{"maximum":16777216,"minimum":1,"nesting_limit":256,"normalization":"none","numbers":"JSON_number_grammar_preserve_lexeme","object_keys":"preserve_order_and_duplicates","representation":"well_formed_UTF8_JSON_document","strings":"scalar_UTF8_and_paired_UTF16_escapes","transport_width":0,"whitespace":"JSON_whitespace_only"},"codec_generation":1,"codec_id":"datatype.json.utf8.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d73f","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d73d","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.json_document.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d73e","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"list","canonical_value_bytes":{"element_types":"TEXT_only","header":"magic8_then_u32LE_count","item":"u8_state_then_u32LE_length_then_UTF8_bytes","magic":"SBTL0001","maximum":16777216,"minimum":12,"normalization":"none","null":"zero_length","representation":"nullable_TEXT_list","states":"zero_null_one_value","trailing_bytes":"refuse","transport_width":0},"codec_generation":1,"codec_id":"datatype.list.text.framed.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d742","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"019d0000-0000-7000-8000-00000000d740","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.list.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d741","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"binary","canonical_value_bytes":{"byte_order":"byte_sequence","empty":"value_present_with_zero_bytes_distinct_from_SQL_NULL","maximum":16777216,"minimum":0,"normalization":"none","representation":"exact_octets_without_text_conversion","transport_width":0},"canonicalization_policy_generation":1,"canonicalization_policy_uuid":"01a0fea5-8a12-7466-9adb-e25464c65b6a","codec_generation":1,"codec_id":"datatype.binary.octets.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d744","codec_version":1,"descriptor_generation":1,"descriptor_identity":"retained_exact_existing_Core_binary_manifest_identity_not_runtime_name_derivation","descriptor_policy_generation":1,"descriptor_policy_uuid":"01a0fea5-8a12-7da5-9d9d-839bc81b09ca","descriptor_uuid":"2d010000-6269-7e61-b279-000000000000","hash_policy_generation":1,"hash_policy_uuid":"01a0fea5-8a12-737b-a482-e696fe7141f8","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","ordering_policy_generation":1,"ordering_policy_uuid":"01a0fea5-8a12-7073-a5b7-32606cd091e6","row_id":"datatype.binary.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d743","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"int8","canonical_value_bytes":{"byte_order":"little_endian","exact":1,"maximum":1,"minimum":1,"representation":"twos_complement_integer"},"codec_generation":1,"codec_id":"datatype.int8.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d801","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"64000000-696e-7438-8000-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.int8.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d800","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"int16","canonical_value_bytes":{"byte_order":"little_endian","exact":2,"maximum":2,"minimum":2,"representation":"twos_complement_integer"},"codec_generation":1,"codec_id":"datatype.int16.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d803","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"65000000-696e-7431-b600-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.int16.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d802","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uint8","canonical_value_bytes":{"byte_order":"little_endian","exact":1,"maximum":1,"minimum":1,"representation":"unsigned_integer"},"codec_generation":1,"codec_id":"datatype.uint8.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d805","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"78000000-7569-7e74-b800-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uint8.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d804","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uint16","canonical_value_bytes":{"byte_order":"little_endian","exact":2,"maximum":2,"minimum":2,"representation":"unsigned_integer"},"codec_generation":1,"codec_id":"datatype.uint16.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d807","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"79000000-7569-7e74-b136-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uint16.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d806","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uint32","canonical_value_bytes":{"byte_order":"little_endian","exact":4,"maximum":4,"minimum":4,"representation":"unsigned_integer"},"codec_generation":1,"codec_id":"datatype.uint32.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d809","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"7a000000-7569-7e74-b332-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uint32.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d808","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"uint128","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"unsigned_integer"},"codec_generation":1,"codec_id":"datatype.uint128.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d80b","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"7c000000-7569-7e74-b132-380000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.uint128.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d80a","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"bfloat16","canonical_value_bytes":{"byte_order":"little_endian","exact":2,"maximum":2,"minimum":2,"representation":"bfloat16_bits"},"codec_generation":1,"codec_id":"datatype.bfloat16.ieee754.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d80d","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"8a000000-6266-7c6f-a174-313600000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.bfloat16.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d80c","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"real16","canonical_value_bytes":{"byte_order":"little_endian","exact":2,"maximum":2,"minimum":2,"representation":"IEEE754_binary16"},"codec_generation":1,"codec_id":"datatype.real16.ieee754.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d80f","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"8b000000-7265-716c-b136-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.real16.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d80e","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"real32","canonical_value_bytes":{"byte_order":"little_endian","exact":4,"maximum":4,"minimum":4,"representation":"IEEE754_binary32"},"codec_generation":1,"codec_id":"datatype.real32.ieee754.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d811","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"8c000000-7265-716c-b332-000000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.real32.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d810","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"real128","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"IEEE754_binary128"},"codec_generation":1,"codec_id":"datatype.real128.ieee754.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d813","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"8e000000-7265-716c-b132-380000000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.real128.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d812","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"ip_address","canonical_value_bytes":{"byte_order":"byte_sequence","exact":16,"maximum":16,"minimum":16,"representation":"IPv6_network_order_with_IPv4_mapped_prefix"},"codec_generation":1,"codec_id":"datatype.ip_address.network.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d815","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"d2000000-6970-7f61-a464-726573730000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.ip_address.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d814","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"network_prefix","canonical_value_bytes":{"byte_order":"byte_sequence","exact":18,"maximum":18,"minimum":18,"representation":"address16_prefix_length_u8_address_family_u8"},"codec_generation":1,"codec_id":"datatype.network_prefix.network.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d817","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"d3000000-071d-7477-af72-6b5f70726566","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.network_prefix.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d816","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"mac_address","canonical_value_bytes":{"byte_order":"byte_sequence","exact":8,"maximum":8,"minimum":8,"representation":"eight_network_order_octets_six_octet_values_zero_extended"},"codec_generation":1,"codec_id":"datatype.mac_address.network.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d819","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"d4000000-6d61-735f-a164-647265737300","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.mac_address.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d818","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"enum_value","canonical_value_bytes":{"byte_order":"byte_sequence","exact":16,"maximum":16,"minimum":16,"representation":"descriptor_bound_member_UUID_octets"},"codec_generation":1,"codec_id":"datatype.enum_value.binary16.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d81b","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"6c020000-656e-756d-9f76-616c75650000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.enum_value.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d81a","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"date","canonical_value_bytes":{"byte_order":"little_endian","exact":4,"maximum":4,"minimum":4,"representation":"signed_i32_days_since_Unix_epoch"},"canonicalization_policy_generation":1,"canonicalization_policy_uuid":"01a1008e-b7f1-72b9-ba08-95b604caebf3","codec_generation":1,"codec_id":"datatype.date.days.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d81d","codec_version":1,"descriptor_generation":1,"descriptor_policy_generation":1,"descriptor_policy_uuid":"01a1008e-b7f0-7913-9a16-000409f9ffb1","descriptor_uuid":"90010000-6461-7465-8000-000000000000","hash_policy_generation":1,"hash_policy_uuid":"01a1008e-b7f3-7a61-a159-5ddb1cbc1df6","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","operation_policy_generation":1,"operation_policy_uuid":"01a1008e-b7f6-7b7a-8ee0-ffbacbcd7dff","ordering_policy_generation":1,"ordering_policy_uuid":"01a1008e-b7f2-7feb-85a8-2d74fe2fde38","row_id":"datatype.date.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d81c","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"time","canonical_value_bytes":{"byte_order":"little_endian","exact":8,"maximum":8,"minimum":8,"representation":"unsigned_u64_nanoseconds_since_midnight"},"canonicalization_policy_generation":1,"canonicalization_policy_uuid":"01a1032b-9f51-7229-977b-45730f3dff35","codec_generation":1,"codec_id":"datatype.time.nanos.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d81f","codec_version":1,"descriptor_generation":1,"descriptor_policy_generation":1,"descriptor_policy_uuid":"01a1032b-9f51-7229-977b-45730f3dff34","descriptor_uuid":"91010000-7469-7d65-8000-000000000000","hash_policy_generation":1,"hash_policy_uuid":"01a1032b-9f51-7229-977b-45730f3dff37","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","operation_policy_generation":1,"operation_policy_uuid":"01a1032b-9f51-7229-977b-45730f3dff3a","ordering_policy_generation":1,"ordering_policy_uuid":"01a1032b-9f51-7229-977b-45730f3dff36","row_id":"datatype.time.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d81e","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"timestamp","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"i64_Unix_seconds_u32_nanoseconds_u32_reserved_zero"},"codec_generation":1,"codec_id":"datatype.timestamp.utc_tuple.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d821","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"92010000-7469-7d65-b374-616d70000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.timestamp.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d820","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"interval","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"i32_months_i32_days_i64_nanoseconds"},"codec_generation":1,"codec_id":"datatype.interval.tuple.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d823","codec_version":1,"descriptor_generation":1,"descriptor_uuid":"93010000-696e-7465-b276-616c00000000","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","row_id":"datatype.interval.v1","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d822","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"decimal_float","canonical_value_bytes":{"byte_order":"little_endian","exact":16,"maximum":16,"minimum":16,"representation":"IEEE754_decimal128_canonical_BID"},"codec_generation":1,"codec_id":"datatype.decimal128.bid.le.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d825","codec_version":1,"comparison_policy_generation":1,"comparison_policy_uuid":"019d0000-0000-7000-8000-00000000d828","comparison_profile":"decimal128_numeric_total_nan_last_v1","descriptor_generation":1,"descriptor_identity":"retained_exact_existing_manifest_identity_as_constant_not_runtime_name_derivation","descriptor_uuid":"a1000000-1065-7369-ad61-6c5f666c6f61","equality_and_hash":"equal_finite_cohorts_all_signed_quantum_zeros_and_all_NaNs_share_the_exact_same_21_byte_comparison_key","invalid_encoding_diagnostic_id":"DATATYPE.DESCRIPTOR.INVALID","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","numeric_context":"precision34_quantum_minus6176_through6111_exact_conversion_no_rounding_or_inexact_substitution","numeric_context_generation":1,"numeric_context_uuid":"019d0000-0000-7000-8000-00000000d826","physical_authority":"DPE-DECIMAL128-BID-EXACT-CODEC-V1","row_id":"datatype.decimal_float.bid.v1","special_value_policy_generation":1,"special_value_policy_uuid":"019d0000-0000-7000-8000-00000000d827","special_values":"canonical_infinities_quiet_and_signaling_NaNs_allowed_sign_class_payload_preserved_in_storage","type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d824","unique_index":"uses_comparison_equivalence_not_raw_BID_representation_SQL_NULL_remains_external_state","visibility":"authenticated_statement_descriptor_projection"},{"canonical_name":"bit_string","canonical_type_code":302,"canonical_value_bytes":{"byte_order":"u32_logical_bit_count_little_endian_then_byte_sequence","empty_present":"00000000","logical_bit_maximum":16777216,"logical_bit_minimum":0,"maximum":2097156,"minimum":4,"representation":"logical_count_then_MSB_first_packed_bits_unused_low_tail_zero","transport_width":0,"variable_width":true},"canonicalization_policy_generation":1,"canonicalization_policy_uuid":"01a0ff27-2716-7c83-9ae4-23bbc73e6a9d","codec_generation":1,"codec_id":"datatype.bit_string.msb0.packed.v1","codec_uuid":"019d0000-0000-7000-8000-00000000d82b","codec_version":1,"descriptor_generation":1,"descriptor_policy_generation":1,"descriptor_policy_uuid":"01a0ff27-2715-75d2-98fb-c853527d3811","descriptor_uuid":"019d0000-0000-7000-8000-00000000d829","hash_policy_generation":1,"hash_policy_uuid":"01a0ff27-2718-7e29-b4b3-7de1719709b8","invalid_encoding_diagnostic_id":"CTB.BIT.CANONICAL_ENCODING_INVALID","lifecycle":"receipt_catalog_snapshot_bound","null_encoding":"containing_slot_value_or_null_state","operation_policy_generation":1,"operation_policy_uuid":"01a0ff27-271b-74de-8bc6-d6a93484ac36","ordering_policy_generation":1,"ordering_policy_uuid":"01a0ff27-2717-7a54-bdbc-a3c1fee7c5e3","row_id":"datatype.bit_string.msb0.packed.v1","sql_null_value_payload_bytes":0,"type_generation":1,"type_uuid":"019d0000-0000-7000-8000-00000000d82a","visibility":"authenticated_statement_descriptor_projection"}])CORE_D708_JSON";

const std::array<CoreD708RowFixture, 33> kCoreD708Rows{{
    CoreD708RowFixture{"boolean", "01000000-626f-7f6c-a561-6e0000000000", 1, "01000000-626f-7f6c-a561-6e0000000000", 1, "01a1010b-2e50-73c3-bdc8-ca82fc1fae5c", "datatype.boolean.u8.v1", 1, 1, 1, true, 1, 1, 1, false, false, true, "single_byte", true, false, "exact_00_FALSE_or_01_TRUE_all_other_bytes_invalid", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"int32", "019d0000-0000-7000-8000-00000000d716", 1, "019d0000-0000-7000-8000-00000000d717", 1, "01a1010b-2e51-7c10-b90a-af9f08d9cd79", "datatype.int32.le.v1", 1, 1, 4, true, 4, 4, 4, false, false, true, "little_endian", true, true, "twos_complement", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"bigint", "019d0000-0000-7000-8000-00000000d711", 1, "019d0000-0000-7000-8000-00000000d712", 1, "01a1010b-2e52-79a4-8669-a9a7cb89bd21", "datatype.int64.le.v1", 1, 1, 8, false, 8, 8, 8, false, false, true, "little_endian", true, true, "twos_complement", "", false, false, false, false, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"decimal", "a0000000-6465-7369-ad61-6c0000000000", 1, "019d0000-0000-7000-8000-00000000d713", 1, "01a1010b-2e54-777f-8089-a807f43084c2", "datatype.decimal.base1e9.le.v1", 1, 1, 24, false, 24, 24, 24, false, false, true, "little_endian", true, true, "exact_decimal_header_and_five_base1e9_coefficient_groups", "", false, false, false, false, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"int128", "019d0000-0000-7000-8000-00000000d714", 1, "019d0000-0000-7000-8000-00000000d715", 1, "01a1010b-2e53-7fef-b6fa-1d9300cd10c8", "datatype.int128.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", true, true, "twos_complement", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"text", "019d0000-0000-7000-8000-00000000d718", 1, "019d0000-0000-7000-8000-00000000d719", 1, "019d0000-0000-7000-8000-00000000d71a", "datatype.text.utf8.v1", 1, 1, 0, true, 0, 16777216, 0, true, true, true, "byte_sequence", true, false, "exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization", "UTF-8", true, true, true, true, true, true, "CTB.TEXT.INVALID_ENCODING", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"real64", "019d0000-0000-7000-8000-00000000d731", 1, "019d0000-0000-7000-8000-00000000d732", 1, "019d0000-0000-7000-8000-00000000d733", "datatype.real64.ieee754.le.v1", 1, 1, 8, true, 8, 8, 8, false, false, true, "little_endian", false, false, "IEEE754_binary64", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uuid", "019d0000-0000-7000-8000-00000000d734", 1, "019d0000-0000-7000-8000-00000000d735", 1, "019d0000-0000-7000-8000-00000000d736", "datatype.uuid.binary16.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "byte_sequence", false, false, "sixteen_UUID_value_octets_without_text_conversion", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"geometry", "019d0000-0000-7000-8000-00000000d737", 1, "019d0000-0000-7000-8000-00000000d738", 1, "019d0000-0000-7000-8000-00000000d739", "datatype.geometry.sbp1.v1", 1, 1, 0, true, 24, 24, 0, false, true, true, "byte_sequence", false, false, "SBP1_finite_2D_point_big_endian_coordinates", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uint64", "019d0000-0000-7000-8000-00000000d73a", 1, "019d0000-0000-7000-8000-00000000d73b", 1, "019d0000-0000-7000-8000-00000000d73c", "datatype.uint64.le.v1", 1, 1, 8, true, 8, 8, 8, false, false, true, "little_endian", true, false, "unsigned_64_bit_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"json_document", "019d0000-0000-7000-8000-00000000d73d", 1, "019d0000-0000-7000-8000-00000000d73e", 1, "019d0000-0000-7000-8000-00000000d73f", "datatype.json.utf8.v1", 1, 1, 0, true, 1, 16777216, 0, true, true, false, "", false, false, "well_formed_UTF8_JSON_document", "", false, false, false, true, true, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"list", "019d0000-0000-7000-8000-00000000d740", 1, "019d0000-0000-7000-8000-00000000d741", 1, "019d0000-0000-7000-8000-00000000d742", "datatype.list.text.framed.v1", 1, 1, 0, true, 12, 16777216, 0, true, true, false, "", false, false, "nullable_TEXT_list", "", false, false, false, true, true, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"binary", "2d010000-6269-7e61-b279-000000000000", 1, "019d0000-0000-7000-8000-00000000d743", 1, "019d0000-0000-7000-8000-00000000d744", "datatype.binary.octets.v1", 1, 1, 0, true, 0, 16777216, 0, true, true, true, "byte_sequence", false, false, "exact_octets_without_text_conversion", "", false, false, true, true, true, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "01a0fea5-8a12-7da5-9d9d-839bc81b09ca", 1, "01a0fea5-8a12-7466-9adb-e25464c65b6a", 1, "01a0fea5-8a12-7073-a5b7-32606cd091e6", 1, "01a0fea5-8a12-737b-a482-e696fe7141f8", 1, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"int8", "64000000-696e-7438-8000-000000000000", 1, "019d0000-0000-7000-8000-00000000d800", 1, "019d0000-0000-7000-8000-00000000d801", "datatype.int8.le.v1", 1, 1, 1, true, 1, 1, 1, false, false, true, "little_endian", false, false, "twos_complement_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"int16", "65000000-696e-7431-b600-000000000000", 1, "019d0000-0000-7000-8000-00000000d802", 1, "019d0000-0000-7000-8000-00000000d803", "datatype.int16.le.v1", 1, 1, 2, true, 2, 2, 2, false, false, true, "little_endian", false, false, "twos_complement_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uint8", "78000000-7569-7e74-b800-000000000000", 1, "019d0000-0000-7000-8000-00000000d804", 1, "019d0000-0000-7000-8000-00000000d805", "datatype.uint8.le.v1", 1, 1, 1, true, 1, 1, 1, false, false, true, "little_endian", false, false, "unsigned_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uint16", "79000000-7569-7e74-b136-000000000000", 1, "019d0000-0000-7000-8000-00000000d806", 1, "019d0000-0000-7000-8000-00000000d807", "datatype.uint16.le.v1", 1, 1, 2, true, 2, 2, 2, false, false, true, "little_endian", false, false, "unsigned_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uint32", "7a000000-7569-7e74-b332-000000000000", 1, "019d0000-0000-7000-8000-00000000d808", 1, "019d0000-0000-7000-8000-00000000d809", "datatype.uint32.le.v1", 1, 1, 4, true, 4, 4, 4, false, false, true, "little_endian", false, false, "unsigned_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"uint128", "7c000000-7569-7e74-b132-380000000000", 1, "019d0000-0000-7000-8000-00000000d80a", 1, "019d0000-0000-7000-8000-00000000d80b", "datatype.uint128.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", false, false, "unsigned_integer", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"bfloat16", "8a000000-6266-7c6f-a174-313600000000", 1, "019d0000-0000-7000-8000-00000000d80c", 1, "019d0000-0000-7000-8000-00000000d80d", "datatype.bfloat16.ieee754.le.v1", 1, 1, 2, true, 2, 2, 2, false, false, true, "little_endian", false, false, "bfloat16_bits", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"real16", "8b000000-7265-716c-b136-000000000000", 1, "019d0000-0000-7000-8000-00000000d80e", 1, "019d0000-0000-7000-8000-00000000d80f", "datatype.real16.ieee754.le.v1", 1, 1, 2, true, 2, 2, 2, false, false, true, "little_endian", false, false, "IEEE754_binary16", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"real32", "8c000000-7265-716c-b332-000000000000", 1, "019d0000-0000-7000-8000-00000000d810", 1, "019d0000-0000-7000-8000-00000000d811", "datatype.real32.ieee754.le.v1", 1, 1, 4, true, 4, 4, 4, false, false, true, "little_endian", false, false, "IEEE754_binary32", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"real128", "8e000000-7265-716c-b132-380000000000", 1, "019d0000-0000-7000-8000-00000000d812", 1, "019d0000-0000-7000-8000-00000000d813", "datatype.real128.ieee754.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", false, false, "IEEE754_binary128", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"ip_address", "d2000000-6970-7f61-a464-726573730000", 1, "019d0000-0000-7000-8000-00000000d814", 1, "019d0000-0000-7000-8000-00000000d815", "datatype.ip_address.network.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "byte_sequence", false, false, "IPv6_network_order_with_IPv4_mapped_prefix", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"network_prefix", "d3000000-071d-7477-af72-6b5f70726566", 1, "019d0000-0000-7000-8000-00000000d816", 1, "019d0000-0000-7000-8000-00000000d817", "datatype.network_prefix.network.v1", 1, 1, 18, true, 18, 18, 18, false, false, true, "byte_sequence", false, false, "address16_prefix_length_u8_address_family_u8", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"mac_address", "d4000000-6d61-735f-a164-647265737300", 1, "019d0000-0000-7000-8000-00000000d818", 1, "019d0000-0000-7000-8000-00000000d819", "datatype.mac_address.network.v1", 1, 1, 8, true, 8, 8, 8, false, false, true, "byte_sequence", false, false, "eight_network_order_octets_six_octet_values_zero_extended", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"enum_value", "6c020000-656e-756d-9f76-616c75650000", 1, "019d0000-0000-7000-8000-00000000d81a", 1, "019d0000-0000-7000-8000-00000000d81b", "datatype.enum_value.binary16.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "byte_sequence", false, false, "descriptor_bound_member_UUID_octets", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"date", "90010000-6461-7465-8000-000000000000", 1, "019d0000-0000-7000-8000-00000000d81c", 1, "019d0000-0000-7000-8000-00000000d81d", "datatype.date.days.le.v1", 1, 1, 4, true, 4, 4, 4, false, false, true, "little_endian", false, false, "signed_i32_days_since_Unix_epoch", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "01a1008e-b7f0-7913-9a16-000409f9ffb1", 1, "01a1008e-b7f1-72b9-ba08-95b604caebf3", 1, "01a1008e-b7f2-7feb-85a8-2d74fe2fde38", 1, "01a1008e-b7f3-7a61-a159-5ddb1cbc1df6", 1, "01a1008e-b7f6-7b7a-8ee0-ffbacbcd7dff", 1},
    CoreD708RowFixture{"time", "91010000-7469-7d65-8000-000000000000", 1, "019d0000-0000-7000-8000-00000000d81e", 1, "019d0000-0000-7000-8000-00000000d81f", "datatype.time.nanos.le.v1", 1, 1, 8, true, 8, 8, 8, false, false, true, "little_endian", false, false, "unsigned_u64_nanoseconds_since_midnight", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "01a1032b-9f51-7229-977b-45730f3dff34", 1, "01a1032b-9f51-7229-977b-45730f3dff35", 1, "01a1032b-9f51-7229-977b-45730f3dff36", 1, "01a1032b-9f51-7229-977b-45730f3dff37", 1, "01a1032b-9f51-7229-977b-45730f3dff3a", 1},
    CoreD708RowFixture{"timestamp", "92010000-7469-7d65-b374-616d70000000", 1, "019d0000-0000-7000-8000-00000000d820", 1, "019d0000-0000-7000-8000-00000000d821", "datatype.timestamp.utc_tuple.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", false, false, "i64_Unix_seconds_u32_nanoseconds_u32_reserved_zero", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"interval", "93010000-696e-7465-b276-616c00000000", 1, "019d0000-0000-7000-8000-00000000d822", 1, "019d0000-0000-7000-8000-00000000d823", "datatype.interval.tuple.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", false, false, "i32_months_i32_days_i64_nanoseconds", "", false, false, false, true, false, false, "", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"decimal_float", "a1000000-1065-7369-ad61-6c5f666c6f61", 1, "019d0000-0000-7000-8000-00000000d824", 1, "019d0000-0000-7000-8000-00000000d825", "datatype.decimal128.bid.le.v1", 1, 1, 16, true, 16, 16, 16, false, false, true, "little_endian", false, false, "IEEE754_decimal128_canonical_BID", "", false, false, false, true, false, true, "DATATYPE.DESCRIPTOR.INVALID", "019d0000-0000-7000-8000-00000000d826", 1, "019d0000-0000-7000-8000-00000000d827", 1, "019d0000-0000-7000-8000-00000000d828", 1, "decimal128_numeric_total_nan_last_v1", true, false, 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0},
    CoreD708RowFixture{"bit_string", "019d0000-0000-7000-8000-00000000d829", 1, "019d0000-0000-7000-8000-00000000d82a", 1, "019d0000-0000-7000-8000-00000000d82b", "datatype.bit_string.msb0.packed.v1", 1, 1, 0, true, 4, 2097156, 0, true, true, true, "u32_logical_bit_count_little_endian_then_byte_sequence", false, false, "logical_count_then_MSB_first_packed_bits_unused_low_tail_zero", "", false, false, true, true, true, true, "CTB.BIT.CANONICAL_ENCODING_INVALID", "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "00000000-0000-0000-0000-000000000000", 0, "", false, true, 302, "01a0ff27-2715-75d2-98fb-c853527d3811", 1, "01a0ff27-2716-7c83-9ae4-23bbc73e6a9d", 1, "01a0ff27-2717-7a54-bdbc-a3c1fee7c5e3", 1, "01a0ff27-2718-7e29-b4b3-7de1719709b8", 1, "01a0ff27-271b-74de-8bc6-d6a93484ac36", 1},
}};


scratchbird::core::platform::Uuid RuntimeFixtureUuid(std::string_view text) {
  Check(text.size() == 36, "Core fixture UUID has an invalid extent");
  scratchbird::core::platform::Uuid value{};
  const auto hex = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    Fail("Core fixture UUID has a non-hexadecimal digit");
  };
  unsigned nibble = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      Check(text[index] == '-', "Core fixture UUID has a bad separator");
      continue;
    }
    const auto digit = hex(text[index]);
    value.bytes[nibble / 2] |= static_cast<Byte>(
        digit << ((nibble % 2) == 0 ? 4 : 0));
    ++nibble;
  }
  Check(nibble == 32, "Core fixture UUID has a bad digit count");
  return value;
}

bool SameLegacyIdentity(const dt::DatatypeTypeCodecIdentityRowV1& left,
                        const dt::DatatypeTypeCodecIdentityRowV1& right) {
  return left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.registry_generation == right.registry_generation &&
         left.descriptor_uuid == right.descriptor_uuid &&
         left.descriptor_generation == right.descriptor_generation &&
         left.type_uuid == right.type_uuid &&
         left.type_generation == right.type_generation &&
         left.codec_id == right.codec_id &&
         left.codec_version == right.codec_version &&
         left.codec_generation == right.codec_generation &&
         left.canonical_value_bytes == right.canonical_value_bytes &&
         left.null_supported == right.null_supported &&
         left.canonical_name == right.canonical_name &&
         left.datatype_identity_code == right.datatype_identity_code &&
         left.null_encoding_code == right.null_encoding_code &&
         left.byte_order_code == right.byte_order_code &&
         left.signed_code == right.signed_code &&
         left.representation_code == right.representation_code &&
         left.codec_uuid == right.codec_uuid &&
         left.canonical_value_minimum_bytes == right.canonical_value_minimum_bytes &&
         left.canonical_value_maximum_bytes == right.canonical_value_maximum_bytes &&
         left.canonical_value_exact_bytes == right.canonical_value_exact_bytes &&
         left.canonical_binary_type_code == right.canonical_binary_type_code &&
         left.canonical_value_variable_width ==
             right.canonical_value_variable_width &&
         left.canonical_value_exact_zero_is_width_marker ==
             right.canonical_value_exact_zero_is_width_marker &&
         left.canonical_byte_order == right.canonical_byte_order &&
         left.canonical_representation == right.canonical_representation &&
         left.canonical_charset == right.canonical_charset &&
         left.shortest_form_utf8_required ==
             right.shortest_form_utf8_required &&
         left.implicit_normalization_allowed ==
             right.implicit_normalization_allowed &&
         left.descriptor_bound_collation_required ==
             right.descriptor_bound_collation_required &&
         left.empty_value_distinct_from_sql_null ==
             right.empty_value_distinct_from_sql_null &&
         left.sql_null_requires_zero_payload ==
             right.sql_null_requires_zero_payload &&
         left.variable_width_storage_without_truncation ==
             right.variable_width_storage_without_truncation &&
         left.invalid_encoding_diagnostic_id == right.invalid_encoding_diagnostic_id &&
         left.numeric_context_uuid == right.numeric_context_uuid &&
         left.numeric_context_generation == right.numeric_context_generation &&
         left.special_value_policy_uuid == right.special_value_policy_uuid &&
         left.special_value_policy_generation ==
             right.special_value_policy_generation &&
         left.comparison_policy_uuid == right.comparison_policy_uuid &&
         left.comparison_policy_generation ==
             right.comparison_policy_generation &&
         left.comparison_profile == right.comparison_profile &&
         left.allow_special_values == right.allow_special_values;
}

bool SamePolicy(const dt::DatatypePolicyIdentityV3& left,
                const dt::DatatypePolicyIdentityV3& right) {
  return left.uuid == right.uuid && left.generation == right.generation;
}

bool SameNative(const dt::DatatypeNativeIdentityFieldsV3& left,
                const dt::DatatypeNativeIdentityFieldsV3& right) {
  return left.present == right.present &&
         left.canonical_value_minimum_bytes ==
             right.canonical_value_minimum_bytes &&
         left.canonical_value_maximum_bytes ==
             right.canonical_value_maximum_bytes &&
         left.canonical_value_transport_width ==
             right.canonical_value_transport_width &&
         left.canonical_value_variable_width ==
             right.canonical_value_variable_width &&
         left.policy_profile_uuid == right.policy_profile_uuid &&
         left.policy_profile_generation == right.policy_profile_generation &&
         left.profile_fingerprint_sha256 == right.profile_fingerprint_sha256;
}

bool SameV3Identity(const dt::DatatypeTypeCodecIdentityRowV3& left,
                    const dt::DatatypeTypeCodecIdentityRowV3& right) {
  return SameLegacyIdentity(left.legacy_fields, right.legacy_fields) &&
         SamePolicy(left.descriptor_policy, right.descriptor_policy) &&
         SamePolicy(left.canonicalization_policy,
                    right.canonicalization_policy) &&
         SamePolicy(left.ordering_policy, right.ordering_policy) &&
         SamePolicy(left.hash_policy, right.hash_policy) &&
         SamePolicy(left.operation_policy, right.operation_policy) &&
         SameNative(left.native_fields, right.native_fields);
}

void AppendU64(std::vector<Byte>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<Byte>((value >> shift) & 0xffu));
  }
}

void AppendU32(std::vector<Byte>* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<Byte>((value >> shift) & 0xffu));
  }
}

void AppendU16(std::vector<Byte>* out, std::uint16_t value) {
  out->push_back(static_cast<Byte>(value & 0xffu));
  out->push_back(static_cast<Byte>((value >> 8) & 0xffu));
}

void AppendBool(std::vector<Byte>* out, bool value) {
  out->push_back(value ? 1 : 0);
}

void AppendUuid(std::vector<Byte>* out,
                const scratchbird::core::platform::Uuid& value) {
  out->insert(out->end(), value.bytes.begin(), value.bytes.end());
}

void AppendString(std::vector<Byte>* out, const std::string& value) {
  AppendU64(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLegacy(std::vector<Byte>* out,
                  const dt::DatatypeTypeCodecIdentityRowV1& row) {
  AppendUuid(out, row.catalog_snapshot_uuid);
  AppendU64(out, row.catalog_generation);
  AppendU64(out, row.registry_generation);
  AppendUuid(out, row.descriptor_uuid);
  AppendU64(out, row.descriptor_generation);
  AppendUuid(out, row.type_uuid);
  AppendU64(out, row.type_generation);
  AppendString(out, row.codec_id);
  AppendU16(out, row.codec_version);
  AppendU64(out, row.codec_generation);
  AppendU32(out, row.canonical_value_bytes);
  AppendBool(out, row.null_supported);
  AppendString(out, row.canonical_name);
  out->push_back(row.datatype_identity_code);
  out->push_back(row.null_encoding_code);
  out->push_back(row.byte_order_code);
  AppendBool(out, row.signed_code);
  out->push_back(row.representation_code);
  AppendU32(out, row.canonical_value_minimum_bytes);
  AppendU32(out, row.canonical_value_maximum_bytes);
  AppendU32(out, row.canonical_value_exact_bytes);
  AppendU32(out, row.canonical_binary_type_code);
  AppendUuid(out, row.codec_uuid);
  AppendBool(out, row.canonical_value_variable_width);
  AppendBool(out, row.canonical_value_exact_zero_is_width_marker);
  AppendString(out, row.canonical_byte_order);
  AppendString(out, row.canonical_representation);
  AppendString(out, row.canonical_charset);
  AppendBool(out, row.shortest_form_utf8_required);
  AppendBool(out, row.implicit_normalization_allowed);
  AppendBool(out, row.descriptor_bound_collation_required);
  AppendBool(out, row.empty_value_distinct_from_sql_null);
  AppendBool(out, row.sql_null_requires_zero_payload);
  AppendBool(out, row.variable_width_storage_without_truncation);
  AppendString(out, row.invalid_encoding_diagnostic_id);
  AppendUuid(out, row.numeric_context_uuid);
  AppendU64(out, row.numeric_context_generation);
  AppendUuid(out, row.special_value_policy_uuid);
  AppendU64(out, row.special_value_policy_generation);
  AppendUuid(out, row.comparison_policy_uuid);
  AppendU64(out, row.comparison_policy_generation);
  AppendString(out, row.comparison_profile);
  AppendBool(out, row.allow_special_values);
}

void AppendPolicy(std::vector<Byte>* out,
                  const dt::DatatypePolicyIdentityV3& policy) {
  AppendUuid(out, policy.uuid);
  AppendU64(out, policy.generation);
}

std::string InternalCompiledMaterialDigest(
    std::span<const dt::DatatypeTypeCodecIdentityRowV3> rows) {
  std::vector<Byte> material;
  const std::string domain =
      "ScratchBird.DatatypeTypeCodecIdentityRegistry.V3.TestOracle.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendU64(&material, rows.size());
  for (const auto& row : rows) {
    AppendLegacy(&material, row.legacy_fields);
    AppendPolicy(&material, row.descriptor_policy);
    AppendPolicy(&material, row.canonicalization_policy);
    AppendPolicy(&material, row.ordering_policy);
    AppendPolicy(&material, row.hash_policy);
    AppendPolicy(&material, row.operation_policy);
  }
  const auto digest =
      scratchbird::core::hash::ComputeSha256Digest(material);
  Check(digest.ok(), "internal compiled-material SHA-256 failed");
  return scratchbird::core::hash::HexLower(digest.digest);
}

void TestCoreD708CanonicalJsonAndCompiledRows() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const Byte*>(kCoreD708RowsCanonicalJson.data()),
      kCoreD708RowsCanonicalJson.size());
  Check(kCoreD708RowsCanonicalJson.size() == 26946,
        "Core d708 canonical JSON byte count changed");
  Check(digest.ok() && scratchbird::core::hash::HexLower(digest.digest) ==
          dt::kDatatypeCohortV8IdentityDigestSha256,
        "Core d708 canonical JSON digest does not match the published seal");

  std::array<const dt::DatatypeTypeCodecIdentityRowV3*, 33> actual{};
  std::size_t count = 0;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (row.legacy_fields.catalog_snapshot_uuid == dt::kDatatypeCohortV8 &&
        row.legacy_fields.catalog_generation == 8 &&
        row.legacy_fields.registry_generation == 8) {
      Check(count < actual.size(), "compiled d708 has excess rows");
      actual[count++] = &row;
    }
  }
  Check(count == actual.size(), "compiled d708 row count differs from Core");

  const auto policy_matches = [&](const dt::DatatypePolicyIdentityV3& value,
                                  const CorePolicyFixture& expected) {
    return value.uuid == RuntimeFixtureUuid(expected.uuid) &&
           value.generation == expected.generation;
  };
  for (std::size_t index = 0; index < actual.size(); ++index) {
    const auto& row = *actual[index];
    const auto& legacy = row.legacy_fields;
    const auto& expected = kCoreD708Rows[index];
    Check(legacy.catalog_snapshot_uuid == dt::kDatatypeCohortV8 &&
              legacy.catalog_generation == 8 &&
              legacy.registry_generation == 8,
          "compiled d708 receipt tuple differs from Core");
    Check(legacy.canonical_name == expected.canonical_name &&
              legacy.descriptor_uuid == RuntimeFixtureUuid(expected.descriptor_uuid) &&
              legacy.descriptor_generation == expected.descriptor_generation &&
              legacy.type_uuid == RuntimeFixtureUuid(expected.type_uuid) &&
              legacy.type_generation == expected.type_generation &&
              legacy.codec_uuid == RuntimeFixtureUuid(expected.codec_uuid) &&
              legacy.codec_id == expected.codec_id &&
              legacy.codec_version == expected.codec_version &&
              legacy.codec_generation == expected.codec_generation,
          "compiled d708 descriptor/type/codec row or declared order differs from Core");
    Check(legacy.canonical_value_bytes == expected.canonical_value_bytes &&
              legacy.null_supported == expected.null_supported &&
              legacy.canonical_value_minimum_bytes == expected.minimum_bytes &&
              legacy.canonical_value_maximum_bytes == expected.maximum_bytes &&
              legacy.canonical_value_exact_bytes == expected.exact_or_transport_bytes &&
              legacy.canonical_value_variable_width == expected.variable_width &&
              legacy.canonical_value_exact_zero_is_width_marker ==
                  expected.exact_zero_is_width_marker &&
              (!expected.has_byte_order ||
               legacy.canonical_byte_order == expected.byte_order) &&
              (!expected.has_signed || legacy.signed_code == expected.signed_value) &&
              legacy.canonical_representation == expected.representation,
          "compiled d708 canonical value semantics differ from Core");
    Check(legacy.canonical_charset == expected.charset &&
              legacy.shortest_form_utf8_required ==
                  expected.shortest_form_utf8_required &&
              legacy.descriptor_bound_collation_required ==
                  expected.descriptor_bound_collation_required &&
              legacy.empty_value_distinct_from_sql_null ==
                  expected.empty_value_distinct_from_sql_null &&
              legacy.sql_null_requires_zero_payload ==
                  expected.sql_null_requires_zero_payload &&
              legacy.variable_width_storage_without_truncation ==
                  expected.variable_width_storage_without_truncation &&
              (!expected.has_invalid_encoding_diagnostic ||
               legacy.invalid_encoding_diagnostic_id ==
                   expected.invalid_encoding_diagnostic_id),
          "compiled d708 text/null/storage semantics differ from Core");
    Check(legacy.numeric_context_uuid ==
                  RuntimeFixtureUuid(expected.numeric_context_uuid) &&
              legacy.numeric_context_generation == expected.numeric_context_generation &&
              legacy.special_value_policy_uuid ==
                  RuntimeFixtureUuid(expected.special_value_policy_uuid) &&
              legacy.special_value_policy_generation ==
                  expected.special_value_policy_generation &&
              legacy.comparison_policy_uuid ==
                  RuntimeFixtureUuid(expected.comparison_policy_uuid) &&
              legacy.comparison_policy_generation ==
                  expected.comparison_policy_generation &&
              legacy.comparison_profile == expected.comparison_profile &&
              legacy.allow_special_values == expected.allow_special_values &&
              (!expected.has_canonical_type_code ||
               legacy.canonical_binary_type_code == expected.canonical_type_code),
          "compiled d708 numeric/comparison semantics differ from Core");
    Check(policy_matches(row.descriptor_policy, expected.descriptor_policy) &&
              policy_matches(row.canonicalization_policy,
                             expected.canonicalization_policy) &&
              policy_matches(row.ordering_policy, expected.ordering_policy) &&
              policy_matches(row.hash_policy, expected.hash_policy) &&
              policy_matches(row.operation_policy, expected.operation_policy),
          "compiled d708 policy identity pair differs from Core");
  }
}

void TestPopulationAndAuthoritativeRows() {
  static_assert(!HasDescriptorPolicyMember<dt::DatatypeTypeCodecIdentityRowV1>);
  static_assert(HasDescriptorPolicyMember<dt::DatatypeTypeCodecIdentityRowV3>);
  static_assert(!std::is_same_v<dt::DatatypePolicyIdentityV1,
                                dt::DatatypePolicyIdentityV3>);

  const auto v3 = dt::CurrentDatatypeTypeCodecIdentityRowsV3();
  Check(v3.size() == 293, "V3 registry does not contain 293 rows");

  std::array<std::size_t, 11> counts{};
  for (const auto& row : v3) {
    const auto generation = row.legacy_fields.catalog_generation;
    Check(generation >= 1 && generation <= 11 &&
              row.legacy_fields.registry_generation == generation,
          "V3 row has a mixed or invalid cohort generation");
    ++counts[generation - 1];
  }
  Check(counts == std::array<std::size_t, 11>{
            6, 12, 13, 31, 32, 33, 33, 33, 33, 33, 34},
        "V3 cohort row counts differ from admitted Core d711");

  Check(dt::kDatatypeCohortV7IdentityDigestSha256 ==
            "f10857ec395d4ebca02ec21c785251a668d98f3c0e69eeba11324f3807832dcc",
        "d707 Core cohort digest binding changed");
  Check(dt::kDatatypeCohortV8IdentityDigestSha256 ==
            "7ff7530978f049864ad10ba5a7a1d4ba78246369ad30be7e7aeb5d7f4261bae5",
        "d708 Core cohort digest binding changed");
  Check(dt::kDatatypeCohortV9IdentityDigestSha256 ==
            "7c3eed94150522b474faa22307a3f94a7753898084de09c67bd108e36fa3a978",
        "d709 Core cohort digest binding changed");
  Check(dt::kDatatypeCohortV10IdentityDigestSha256 ==
            "90d4e17c5e98a684422399b16d73b38d3391905ebb775241426755a7323249a3",
        "d710 Core cohort digest binding changed");
  Check(dt::kDatatypeCohortV11IdentityDigestSha256 ==
            "8d6cb5b855450a355f05863bc2b0c3652d7b694a1e6ebd6758f1a41408840327",
        "d711 Core cohort digest binding changed");

  const auto registry_digest = InternalCompiledMaterialDigest(v3);
  if (registry_digest !=
      "ca07250c01e11301b2bbdb2a26ab8bea68eed7a5910907a5bec56889898dbafc") {
    std::cerr << "observed_internal_compiled_material_digest=" << registry_digest << '\n';
    Fail("internal compiled V3 material changed");
  }

  constexpr std::array<std::string_view, 11> expected_cohort_digests{{
      "c3f32a278b09243fe3556ba9520c6ad95fb0995035813f92244c775b1bc4e3e2",
      "72399142c162281c1f81e1ec64175f546b26baa45f0fd241abab5823d913e166",
      "3ca980ce8e214c713d53bffcfb34e73bc64094065c8e2f93ed264e530cfeb299",
      "e2487d9e772f3e0ddb1a9873dffd70f13f55b930df111efc07e7889eb7581968",
      "d2422a08f9c7f4eb5c5c7cc984c6df54128ce5375a7fde25000c0a9842c436ad",
      "f5b95f6de3b668d5fe325f0016d224d2faa591076327e795e63b0858e35aa2b0",
      "f8f70ee80dbc9e877bd497d7f07d117d99abd994126403b7bfd0f3c684357e3b",
      "2ba35727670308890132276ed9a64fffffe72046690e4a7adf962d2fe02132c2",
      "aaac0121339b61d74613dc9a95b35c7f61a3a32fc780a02f5db2286e9bebaa4d",
      "d70417ccc10c2dd672416901be8cfea511cca6f8f78cc85a0cda0f198d8638ed",
      "1cdd4807c44eb728c367a0bfe652cf84add696ef1a58a7a4e5981fcbafe787ad",
  }};
  for (std::uint64_t generation = 1; generation <= 11; ++generation) {
    std::vector<dt::DatatypeTypeCodecIdentityRowV3> cohort;
    for (const auto& row : v3) {
      if (row.legacy_fields.catalog_generation == generation)
        cohort.push_back(row);
    }
    const auto cohort_digest = InternalCompiledMaterialDigest(cohort);
    if (cohort_digest != expected_cohort_digests[generation - 1]) {
      std::cerr << "generation=" << generation
                << " observed_internal_cohort_material_digest=" << cohort_digest << '\n';
      Fail("internal compiled cohort material changed");
    }
  }

  using scratchbird::tests::FixtureUuidLiteral;
  const auto binary_descriptor =
      FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000");
  const auto bit_descriptor =
      FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829");
  const auto date_descriptor =
      FixtureUuidLiteral("90010000-6461-7465-8000-000000000000");
  const auto time_descriptor =
      FixtureUuidLiteral("91010000-7469-7d65-8000-000000000000");
  const auto timestamp_descriptor =
      FixtureUuidLiteral("92010000-7469-7d65-b374-616d70000000");
  const auto interval_descriptor =
      FixtureUuidLiteral("93010000-696e-7465-b276-616c00000000");
  const auto blob_descriptor =
      FixtureUuidLiteral("016fd1d3-0daf-5967-b4d7-07fe859a418e");
  std::size_t binary_policy_rows = 0;
  std::size_t bit_policy_rows = 0;
  std::size_t date_policy_rows = 0;
  std::size_t time_policy_rows = 0;
  std::size_t timestamp_policy_rows = 0;
  std::size_t interval_policy_rows = 0;
  std::size_t blob_policy_rows = 0;
  for (const auto& row : v3) {
    const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
        row.legacy_fields.catalog_snapshot_uuid,
        row.legacy_fields.catalog_generation,
        row.legacy_fields.registry_generation,
        row.legacy_fields.descriptor_uuid,
        row.legacy_fields.descriptor_generation);
    Check(lookup.ok && SameV3Identity(lookup.row, row),
          "exact V3 tuple did not resolve to its authoritative row");

    if (row.legacy_fields.catalog_generation == 11) {
      Check(row.native_fields.present,
            "d711 row lacks its lossless V3 native identity fields");
      if (row.legacy_fields.descriptor_uuid != blob_descriptor) {
        Check(row.native_fields.canonical_value_minimum_bytes ==
                      row.legacy_fields.canonical_value_minimum_bytes &&
                  row.native_fields.canonical_value_maximum_bytes ==
                      row.legacy_fields.canonical_value_maximum_bytes &&
                  row.native_fields.canonical_value_transport_width ==
                      row.legacy_fields.canonical_value_exact_bytes &&
                  row.native_fields.canonical_value_variable_width ==
                      row.legacy_fields.canonical_value_variable_width &&
                  row.native_fields.policy_profile_uuid.is_nil() &&
                  row.native_fields.policy_profile_generation == 0,
              "d711 predecessor reseal changed its native extent/profile facts");
      }
    } else {
      Check(!row.native_fields.present,
            "historical predecessor acquired V3-native successor authority");
    }

    if (row.legacy_fields.descriptor_uuid == binary_descriptor) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                !row.canonicalization_policy.uuid.is_nil() &&
                row.canonicalization_policy.generation == 1 &&
                !row.ordering_policy.uuid.is_nil() &&
                row.ordering_policy.generation == 1 &&
                !row.hash_policy.uuid.is_nil() &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.uuid.is_nil() &&
                row.operation_policy.generation == 0,
            "base.binary V3 policy authority changed");
      ++binary_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == bit_descriptor) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil(),
            "base.bit_string V3 policy authority is incomplete");
      ++bit_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == date_descriptor &&
               row.legacy_fields.catalog_generation >= 7) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                row.canonicalization_policy.generation == 1 &&
                row.ordering_policy.generation == 1 &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.generation == 1,
            "base.date current policy authority is incomplete");
      ++date_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == time_descriptor &&
               row.legacy_fields.catalog_generation >= 8) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                row.canonicalization_policy.generation == 1 &&
                row.ordering_policy.generation == 1 &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.generation == 1,
            "base.time current policy authority is incomplete");
      ++time_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == timestamp_descriptor &&
               row.legacy_fields.catalog_generation >= 9) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                row.canonicalization_policy.generation == 1 &&
                row.ordering_policy.generation == 1 &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.generation == 1,
            "base.timestamp current policy authority is incomplete");
      ++timestamp_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == interval_descriptor &&
               row.legacy_fields.catalog_generation >= 10) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                row.canonicalization_policy.generation == 1 &&
                row.ordering_policy.generation == 1 &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.generation == 1,
            "base.interval d710 policy authority is incomplete");
      ++interval_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == blob_descriptor &&
               row.legacy_fields.catalog_generation == 11) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                row.canonicalization_policy.generation == 1 &&
                row.ordering_policy.generation == 1 &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.generation == 1,
            "base.blob d711 policy authority is incomplete");
      ++blob_policy_rows;
    } else {
      Check(row.descriptor_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 0 &&
                row.canonicalization_policy.uuid.is_nil() &&
                row.canonicalization_policy.generation == 0 &&
                row.ordering_policy.uuid.is_nil() &&
                row.ordering_policy.generation == 0 &&
                row.hash_policy.uuid.is_nil() &&
                row.hash_policy.generation == 0 &&
                row.operation_policy.uuid.is_nil() &&
                row.operation_policy.generation == 0,
            "V3 row contains an unregistered policy identity");
    }
  }
  Check(binary_policy_rows == 9 && bit_policy_rows == 6 &&
            date_policy_rows == 5 && time_policy_rows == 4 &&
            timestamp_policy_rows == 3 && interval_policy_rows == 2 &&
            blob_policy_rows == 1,
        "V3 policy-bearing row population changed");

  const std::array<std::size_t, 11> expected_inherited{
      0, 6, 12, 13, 31, 32, 27, 25, 32, 32, 33};
  std::array<std::size_t, 11> inherited_counts{};
  for (const auto& current : v3) {
    const auto generation = current.legacy_fields.catalog_generation;
    if (generation == 1) continue;
    for (const auto& predecessor : v3) {
      if (predecessor.legacy_fields.catalog_generation != generation - 1 ||
          predecessor.legacy_fields.descriptor_uuid !=
              current.legacy_fields.descriptor_uuid ||
          predecessor.legacy_fields.descriptor_generation !=
              current.legacy_fields.descriptor_generation) {
        continue;
      }
      auto expected_legacy = predecessor.legacy_fields;
      expected_legacy.catalog_snapshot_uuid =
          current.legacy_fields.catalog_snapshot_uuid;
      expected_legacy.catalog_generation = generation;
      expected_legacy.registry_generation = generation;
      const bool d707_codec_closure = generation == 7 &&
          (current.legacy_fields.canonical_name == "boolean" ||
           current.legacy_fields.canonical_name == "int32" ||
           current.legacy_fields.canonical_name == "bigint" ||
           current.legacy_fields.canonical_name == "decimal" ||
           current.legacy_fields.canonical_name == "int128");
      const bool d707_date_closure = generation == 7 &&
          current.legacy_fields.canonical_name == "date";
      const bool d708_current_materialization = generation == 8 &&
          (current.legacy_fields.canonical_name == "boolean" ||
           current.legacy_fields.canonical_name == "int32" ||
           current.legacy_fields.canonical_name == "bigint" ||
           current.legacy_fields.canonical_name == "decimal" ||
           current.legacy_fields.canonical_name == "int128" ||
           current.legacy_fields.canonical_name == "geometry" ||
           current.legacy_fields.canonical_name == "list" ||
           current.legacy_fields.canonical_name == "time");
      const bool d709_timestamp_materialization = generation == 9 &&
          current.legacy_fields.canonical_name == "timestamp";
      const bool d710_interval_materialization = generation == 10 &&
          current.legacy_fields.canonical_name == "interval";
      if (d707_codec_closure || d707_date_closure ||
          d708_current_materialization || d709_timestamp_materialization ||
          d710_interval_materialization) continue;
      Check(SameLegacyIdentity(expected_legacy, current.legacy_fields) &&
                SamePolicy(predecessor.descriptor_policy,
                           current.descriptor_policy) &&
                SamePolicy(predecessor.canonicalization_policy,
                           current.canonicalization_policy) &&
                SamePolicy(predecessor.ordering_policy,
                           current.ordering_policy) &&
                SamePolicy(predecessor.hash_policy, current.hash_policy) &&
                SamePolicy(predecessor.operation_policy,
                           current.operation_policy),
            "successor cohort altered inherited V3 authority");
      ++inherited_counts[generation - 1];
    }
  }
  Check(inherited_counts == expected_inherited,
        "V3 successor inheritance counts changed");

  const std::array<std::string_view, 5> codec_closure_names{
      "boolean", "int32", "bigint", "decimal", "int128"};
  std::size_t codec_closures = 0;
  for (const auto& row : v3) {
    if (row.legacy_fields.catalog_generation != 7) continue;
    Check(!row.legacy_fields.codec_uuid.is_nil() &&
              row.legacy_fields.codec_uuid != row.legacy_fields.descriptor_uuid &&
              row.legacy_fields.codec_uuid != row.legacy_fields.type_uuid,
          "d707 executable row lacks a distinct codec UUID");
    for (const auto name : codec_closure_names) {
      if (row.legacy_fields.canonical_name == name) ++codec_closures;
    }
  }
  Check(codec_closures == codec_closure_names.size(),
        "d707 codec-identity closure population changed");

  std::vector<const dt::DatatypeTypeCodecIdentityRowV3*> d706;
  std::vector<const dt::DatatypeTypeCodecIdentityRowV3*> d707;
  std::vector<const dt::DatatypeTypeCodecIdentityRowV3*> d708;
  std::vector<const dt::DatatypeTypeCodecIdentityRowV3*> d709;
  std::vector<const dt::DatatypeTypeCodecIdentityRowV3*> d710;
  for (const auto& row : v3) {
    if (row.legacy_fields.catalog_generation == 6) d706.push_back(&row);
    if (row.legacy_fields.catalog_generation == 7) d707.push_back(&row);
    if (row.legacy_fields.catalog_generation == 8) d708.push_back(&row);
    if (row.legacy_fields.catalog_generation == 9) d709.push_back(&row);
    if (row.legacy_fields.catalog_generation == 10) d710.push_back(&row);
  }
  Check(d706.size() == 33 && d707.size() == 33 && d708.size() == 33 &&
            d709.size() == 33 && d710.size() == 33,
        "successor cohorts are incomplete");
  for (std::size_t i = 0; i < d707.size(); ++i) {
    Check(d706[i]->legacy_fields.descriptor_uuid ==
              d707[i]->legacy_fields.descriptor_uuid,
          "d707 row order differs from its declared predecessor order");
    for (std::size_t j = i + 1; j < d707.size(); ++j) {
      Check(d707[i]->legacy_fields.descriptor_uuid !=
                d707[j]->legacy_fields.descriptor_uuid &&
                d707[i]->legacy_fields.type_uuid !=
                    d707[j]->legacy_fields.type_uuid &&
                d707[i]->legacy_fields.codec_uuid !=
                    d707[j]->legacy_fields.codec_uuid,
            "d707 same-role UUID is not globally unique");
    }
  }
  std::size_t cross_role_aliases = 0;
  for (const auto* left : d707) {
    for (const auto* right : d707) {
      if (left->legacy_fields.descriptor_uuid ==
          right->legacy_fields.type_uuid) {
        Check(left->legacy_fields.canonical_name == "boolean" &&
                  right->legacy_fields.canonical_name == "boolean",
              "unapproved d707 descriptor/type cross-role alias");
        ++cross_role_aliases;
      }
      Check(left->legacy_fields.codec_uuid !=
                    right->legacy_fields.descriptor_uuid &&
                left->legacy_fields.codec_uuid !=
                    right->legacy_fields.type_uuid,
            "d707 codec UUID aliases another authority role");
    }
  }
  Check(cross_role_aliases == 1,
        "boolean is not the sole d707 descriptor/type alias");

  for (std::size_t i = 0; i < d708.size(); ++i) {
    Check(d708[i]->legacy_fields.descriptor_uuid ==
              RuntimeFixtureUuid(kCoreD708Rows[i].descriptor_uuid),
          "d708 row order differs from the declared Core order");
    for (std::size_t j = i + 1; j < d708.size(); ++j) {
      Check(d708[i]->legacy_fields.descriptor_uuid !=
                d708[j]->legacy_fields.descriptor_uuid &&
                d708[i]->legacy_fields.type_uuid !=
                    d708[j]->legacy_fields.type_uuid &&
                d708[i]->legacy_fields.codec_uuid !=
                    d708[j]->legacy_fields.codec_uuid,
            "d708 same-role UUID is not globally unique");
    }
  }
  for (std::size_t i = 0; i < d709.size(); ++i) {
    Check(d709[i]->legacy_fields.descriptor_uuid ==
              RuntimeFixtureUuid(kCoreD708Rows[i].descriptor_uuid),
          "d709 row order differs from the declared Core order");
    for (std::size_t j = i + 1; j < d709.size(); ++j) {
      Check(d709[i]->legacy_fields.descriptor_uuid !=
                d709[j]->legacy_fields.descriptor_uuid &&
                d709[i]->legacy_fields.type_uuid !=
                    d709[j]->legacy_fields.type_uuid &&
                d709[i]->legacy_fields.codec_uuid !=
                    d709[j]->legacy_fields.codec_uuid,
            "d709 same-role UUID is not globally unique");
    }
  }
  for (std::size_t i = 0; i < d710.size(); ++i) {
    Check(d710[i]->legacy_fields.descriptor_uuid ==
              RuntimeFixtureUuid(kCoreD708Rows[i].descriptor_uuid),
          "d710 row order differs from the declared Core order");
    for (std::size_t j = i + 1; j < d710.size(); ++j) {
      Check(d710[i]->legacy_fields.descriptor_uuid !=
                d710[j]->legacy_fields.descriptor_uuid &&
                d710[i]->legacy_fields.type_uuid !=
                    d710[j]->legacy_fields.type_uuid &&
                d710[i]->legacy_fields.codec_uuid !=
                    d710[j]->legacy_fields.codec_uuid,
            "d710 same-role UUID is not globally unique");
    }
  }
}

void TestExactBitStringIdentity() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto descriptor = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829");
  const auto type = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d82a");
  const auto codec = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d82b");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, descriptor, 1);
  Check(lookup.ok && dt::IsExactCanonicalBitStringTypeCodecIdentityV3(lookup.row),
        "exact d710 bit-string row did not resolve");
  const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, descriptor, 1);
  Check(historical.ok &&
            !dt::IsExactCanonicalBitStringTypeCodecIdentityV3(historical.row),
        "historical d709 bit-string row was admitted as current");
  const auto& row = lookup.row;
  const auto& legacy = row.legacy_fields;
  Check(legacy.type_uuid == type && legacy.codec_uuid == codec &&
            legacy.codec_id == "datatype.bit_string.msb0.packed.v1" &&
            legacy.canonical_binary_type_code == 302 &&
            legacy.canonical_value_minimum_bytes == 4 &&
            legacy.canonical_value_maximum_bytes == 2097156 &&
            legacy.canonical_value_exact_bytes == 0 &&
            legacy.canonical_value_variable_width &&
            legacy.canonical_value_exact_zero_is_width_marker &&
            legacy.canonical_byte_order ==
                "u32_logical_bit_count_little_endian_then_byte_sequence" &&
            legacy.canonical_representation ==
                "logical_count_then_MSB_first_packed_bits_unused_low_tail_zero" &&
            legacy.invalid_encoding_diagnostic_id ==
                "CTB.BIT.CANONICAL_ENCODING_INVALID",
        "d710 bit-string identity/bounds/representation drifted");
  Check(row.descriptor_policy.uuid == FixtureUuidLiteral("01a0ff27-2715-75d2-98fb-c853527d3811") &&
            row.canonicalization_policy.uuid == FixtureUuidLiteral("01a0ff27-2716-7c83-9ae4-23bbc73e6a9d") &&
            row.ordering_policy.uuid == FixtureUuidLiteral("01a0ff27-2717-7a54-bdbc-a3c1fee7c5e3") &&
            row.hash_policy.uuid == FixtureUuidLiteral("01a0ff27-2718-7e29-b4b3-7de1719709b8") &&
            row.operation_policy.uuid == FixtureUuidLiteral("01a0ff27-271b-74de-8bc6-d6a93484ac36") &&
            row.descriptor_policy.generation == 1 &&
            row.canonicalization_policy.generation == 1 &&
            row.ordering_policy.generation == 1 &&
            row.hash_policy.generation == 1 &&
            row.operation_policy.generation == 1,
        "d710 bit-string policy identity drifted");

  for (unsigned generation = 1; generation <= 5; ++generation) {
    auto snapshot = dt::kDatatypeCohortV8;
    snapshot.bytes.back() = static_cast<std::uint8_t>(generation);
    Check(!dt::LookupDatatypeTypeCodecIdentityV3(
               snapshot, generation, generation, descriptor, 1).ok,
          "bit-string identity leaked into a predecessor cohort");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV10, 9, 10,
                                               descriptor, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV10, 10, 9,
                                                   descriptor, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV9, 10, 10,
                                                   descriptor, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV11, 11, 11,
                                                   type, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV11, 11, 11,
                                                   codec, 1).ok,
        "V3 lookup inferred an identity from a mismatched tuple");

  const auto reject_mutation = [&](auto mutate, std::string_view message) {
    auto changed = row;
    mutate(changed);
    Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed), message);
  };
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_maximum_bytes--; },
                  "altered bit-string maximum admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_representation += "_other"; },
                  "altered bit-string representation admitted");
  reject_mutation([](auto& value) { value.descriptor_policy.generation = 2; },
                  "altered descriptor policy admitted");
  reject_mutation([](auto& value) { value.canonicalization_policy.uuid.bytes[0] ^= 1; },
                  "altered canonicalization policy admitted");
  reject_mutation([](auto& value) { value.ordering_policy.uuid.bytes[15] ^= 1; },
                  "altered ordering policy admitted");
  reject_mutation([](auto& value) { value.hash_policy.generation = 0; },
                  "altered hash policy admitted");
  reject_mutation([](auto& value) { value.operation_policy.uuid.bytes[8] ^= 1; },
                  "altered operation policy admitted");

  // Every field in the policy-bearing V3 row is authority.  Exercise each
  // non-UUID legacy field independently; UUIDs receive the exhaustive
  // single-bit coverage below.
  reject_mutation([](auto& value) { ++value.legacy_fields.catalog_generation; },
                  "altered catalog generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.registry_generation; },
                  "altered registry generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.descriptor_generation; },
                  "altered descriptor generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.type_generation; },
                  "altered type generation admitted");
  auto renamed_codec = row;
  renamed_codec.legacy_fields.codec_id = "packed_bit_sequence_codec";
  Check(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(renamed_codec),
        "renamed bit-string codec label changed exact identity");
  auto translated_codec = row;
  translated_codec.legacy_fields.codec_id = "codec_cadena_de_bits";
  Check(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(translated_codec),
        "translated bit-string codec label changed exact identity");
  reject_mutation([](auto& value) { ++value.legacy_fields.codec_version; },
                  "altered codec version admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.codec_generation; },
                  "altered codec generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_value_bytes; },
                  "altered canonical value width admitted");
  reject_mutation([](auto& value) { value.legacy_fields.null_supported = false; },
                  "altered NULL support admitted");
  auto renamed = row;
  renamed.legacy_fields.canonical_name = "bit_sequence";
  Check(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(renamed),
        "renamed bit-string presentation label changed exact identity");
  auto translated = row;
  translated.legacy_fields.canonical_name = "cadena_de_bits";
  Check(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(translated),
        "translated bit-string presentation label changed exact identity");
  reject_mutation([](auto& value) { ++value.legacy_fields.datatype_identity_code; },
                  "altered datatype identity code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.null_encoding_code; },
                  "altered NULL encoding code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.byte_order_code; },
                  "altered byte-order code admitted");
  reject_mutation([](auto& value) { value.legacy_fields.signed_code = true; },
                  "altered signed code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.representation_code; },
                  "altered representation code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_value_minimum_bytes; },
                  "altered canonical minimum admitted");
  reject_mutation([](auto& value) { --value.legacy_fields.canonical_value_exact_bytes; },
                  "altered canonical exact width admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_binary_type_code; },
                  "altered canonical binary type code admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_variable_width = false; },
                  "altered variable-width flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_exact_zero_is_width_marker = false; },
                  "altered zero-width-marker flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_byte_order += ".other"; },
                  "altered canonical byte order admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_charset = "binary"; },
                  "altered canonical charset admitted");
  reject_mutation([](auto& value) { value.legacy_fields.shortest_form_utf8_required = true; },
                  "altered shortest-form flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.implicit_normalization_allowed = true; },
                  "altered normalization flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.descriptor_bound_collation_required = true; },
                  "altered collation flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.empty_value_distinct_from_sql_null = false; },
                  "altered empty-versus-NULL flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.sql_null_requires_zero_payload = false; },
                  "altered NULL payload rule admitted");
  reject_mutation([](auto& value) { value.legacy_fields.variable_width_storage_without_truncation = false; },
                  "altered storage truncation rule admitted");
  reject_mutation([](auto& value) { value.legacy_fields.invalid_encoding_diagnostic_id += ".other"; },
                  "altered invalid-encoding diagnostic admitted");
  reject_mutation([](auto& value) { value.legacy_fields.numeric_context_uuid.bytes[0] ^= 1; },
                  "altered numeric context UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.numeric_context_generation; },
                  "altered numeric context generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.special_value_policy_uuid.bytes[0] ^= 1; },
                  "altered special-value policy UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.special_value_policy_generation; },
                  "altered special-value policy generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.comparison_policy_uuid.bytes[0] ^= 1; },
                  "altered comparison policy UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.comparison_policy_generation; },
                  "altered comparison policy generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.comparison_profile = "other"; },
                  "altered comparison profile admitted");
  reject_mutation([](auto& value) { value.legacy_fields.allow_special_values = true; },
                  "altered special-value admission admitted");

  using Legacy = dt::DatatypeTypeCodecIdentityRowV1;
  for (const auto member : {&Legacy::catalog_snapshot_uuid,
                            &Legacy::descriptor_uuid,
                            &Legacy::type_uuid,
                            &Legacy::codec_uuid}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = row;
      (changed.legacy_fields.*member).bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1u << (bit % 8));
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "single-bit legacy identity mutation was admitted");
    }
  }
  for (const auto member : {&dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = row;
      (changed.*member).uuid.bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1u << (bit % 8));
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "single-bit policy identity mutation was admitted");
    }
    for (const auto generation : {0ULL, 2ULL, ~0ULL}) {
      auto changed = row;
      (changed.*member).generation = generation;
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "policy generation mutation was admitted");
    }
  }

  const auto projected = dt::ProjectDatatypeTypeCodecIdentityV3ToV1(row);
  Check(projected.ok && projected.diagnostic_id.empty() &&
            SameLegacyIdentity(projected.row, legacy),
        "one-way V3-to-V1 projection changed legacy fields");
}

void TestExactCurrentBinaryDateAndCodecClosures() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto binary_descriptor =
      FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000");
  const auto bit_descriptor =
      FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829");
  const auto date_descriptor =
      FixtureUuidLiteral("90010000-6461-7465-8000-000000000000");
  const auto time_descriptor =
      FixtureUuidLiteral("91010000-7469-7d65-8000-000000000000");
  const auto timestamp_descriptor =
      FixtureUuidLiteral("92010000-7469-7d65-b374-616d70000000");

  const auto binary = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, binary_descriptor, 1);
  const auto bit = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, bit_descriptor, 1);
  const auto date = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, date_descriptor, 1);
  const auto time = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, time_descriptor, 1);
  const auto timestamp = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, timestamp_descriptor, 1);
  Check(binary.ok && dt::IsExactCanonicalBinaryTypeCodecIdentityV3(binary.row),
        "exact current binary identity is absent");
  Check(bit.ok && dt::IsExactCanonicalBitStringTypeCodecIdentityV3(bit.row),
        "exact current bit-string identity is absent");
  Check(date.ok && dt::IsExactCanonicalDateTypeCodecIdentityV3(date.row),
        "exact current date identity is absent");
  Check(time.ok && dt::IsExactCanonicalTimeTypeCodecIdentityV3(time.row),
        "exact current time identity is absent");
  Check(timestamp.ok &&
            dt::IsExactCanonicalTimestampTypeCodecIdentityV3(timestamp.row),
        "exact current timestamp identity is absent");

  const auto historical_binary = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, binary_descriptor, 1);
  const auto historical_bit = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, bit_descriptor, 1);
  const auto historical_date = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, date_descriptor, 1);
  const auto historical_time = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, time_descriptor, 1);
  const auto historical_timestamp = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, timestamp_descriptor, 1);
  Check(historical_binary.ok && historical_bit.ok && historical_date.ok &&
            historical_time.ok && historical_timestamp.ok &&
            !dt::IsExactCanonicalBinaryTypeCodecIdentityV3(
                historical_binary.row) &&
            !dt::IsExactCanonicalBitStringTypeCodecIdentityV3(
                historical_bit.row) &&
            !dt::IsExactCanonicalDateTypeCodecIdentityV3(
                historical_date.row) &&
            !dt::IsExactCanonicalTimeTypeCodecIdentityV3(
                historical_time.row) &&
            !dt::IsExactCanonicalTimestampTypeCodecIdentityV3(
                historical_timestamp.row),
        "historical d709 identities were admitted as current d710");

  auto renamed_binary = binary.row;
  renamed_binary.legacy_fields.canonical_name = "octet_sequence";
  Check(dt::IsExactCanonicalBinaryTypeCodecIdentityV3(renamed_binary),
        "renamed binary presentation label changed exact identity");
  auto translated_binary = binary.row;
  translated_binary.legacy_fields.canonical_name = "secuencia_binaria";
  Check(dt::IsExactCanonicalBinaryTypeCodecIdentityV3(translated_binary),
        "translated binary presentation label changed exact identity");
  auto renamed_binary_codec = binary.row;
  renamed_binary_codec.legacy_fields.codec_id = "octet_sequence_codec";
  Check(dt::IsExactCanonicalBinaryTypeCodecIdentityV3(renamed_binary_codec),
        "renamed binary codec label changed exact identity");
  auto translated_binary_codec = binary.row;
  translated_binary_codec.legacy_fields.codec_id = "codec_secuencia_binaria";
  Check(dt::IsExactCanonicalBinaryTypeCodecIdentityV3(
            translated_binary_codec),
        "translated binary codec label changed exact identity");

  auto renamed_date = date.row;
  renamed_date.legacy_fields.canonical_name = "calendar_date";
  Check(dt::IsExactCanonicalDateTypeCodecIdentityV3(renamed_date),
        "renamed date presentation label changed exact identity");
  auto translated_date = date.row;
  translated_date.legacy_fields.canonical_name = "fecha";
  Check(dt::IsExactCanonicalDateTypeCodecIdentityV3(translated_date),
        "translated date presentation label changed exact identity");
  auto renamed_date_codec = date.row;
  renamed_date_codec.legacy_fields.codec_id = "epoch_day_codec";
  Check(dt::IsExactCanonicalDateTypeCodecIdentityV3(renamed_date_codec),
        "renamed date codec label changed exact identity");
  auto translated_date_codec = date.row;
  translated_date_codec.legacy_fields.codec_id = "codec_dias_desde_epoca";
  Check(dt::IsExactCanonicalDateTypeCodecIdentityV3(translated_date_codec),
        "translated date codec label changed exact identity");

  Check(date.row.legacy_fields.type_uuid ==
            FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81c") &&
            date.row.legacy_fields.codec_uuid ==
            FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81d") &&
            date.row.legacy_fields.codec_id == "datatype.date.days.le.v1" &&
            date.row.legacy_fields.codec_version == 1 &&
            date.row.legacy_fields.codec_generation == 1 &&
            date.row.legacy_fields.canonical_value_minimum_bytes == 4 &&
            date.row.legacy_fields.canonical_value_maximum_bytes == 4 &&
            date.row.legacy_fields.canonical_value_exact_bytes == 4 &&
            date.row.legacy_fields.canonical_binary_type_code == 400 &&
            date.row.legacy_fields.canonical_byte_order == "little_endian" &&
            date.row.legacy_fields.canonical_representation ==
                "signed_i32_days_since_Unix_epoch",
        "current date descriptor/type/codec material drifted");
  Check(date.row.descriptor_policy.uuid ==
            FixtureUuidLiteral("01a1008e-b7f0-7913-9a16-000409f9ffb1") &&
            date.row.canonicalization_policy.uuid ==
            FixtureUuidLiteral("01a1008e-b7f1-72b9-ba08-95b604caebf3") &&
            date.row.ordering_policy.uuid ==
            FixtureUuidLiteral("01a1008e-b7f2-7feb-85a8-2d74fe2fde38") &&
            date.row.hash_policy.uuid ==
            FixtureUuidLiteral("01a1008e-b7f3-7a61-a159-5ddb1cbc1df6") &&
            date.row.operation_policy.uuid ==
            FixtureUuidLiteral("01a1008e-b7f6-7b7a-8ee0-ffbacbcd7dff"),
        "current date policy tuple drifted");

  Check(time.row.legacy_fields.type_uuid ==
            FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81e") &&
            time.row.legacy_fields.codec_uuid ==
            FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81f") &&
            time.row.legacy_fields.canonical_value_minimum_bytes == 8 &&
            time.row.legacy_fields.canonical_value_maximum_bytes == 8 &&
            time.row.legacy_fields.canonical_value_exact_bytes == 8 &&
            time.row.legacy_fields.canonical_binary_type_code == 401 &&
            time.row.legacy_fields.canonical_byte_order == "little_endian" &&
            time.row.legacy_fields.canonical_representation ==
                "unsigned_u64_nanoseconds_since_midnight",
        "current time descriptor/type/codec material drifted");
  Check(time.row.descriptor_policy.uuid ==
            FixtureUuidLiteral("01a1032b-9f51-7229-977b-45730f3dff34") &&
            time.row.canonicalization_policy.uuid ==
            FixtureUuidLiteral("01a1032b-9f51-7229-977b-45730f3dff35") &&
            time.row.ordering_policy.uuid ==
            FixtureUuidLiteral("01a1032b-9f51-7229-977b-45730f3dff36") &&
            time.row.hash_policy.uuid ==
            FixtureUuidLiteral("01a1032b-9f51-7229-977b-45730f3dff37") &&
            time.row.operation_policy.uuid ==
            FixtureUuidLiteral("01a1032b-9f51-7229-977b-45730f3dff3a"),
        "current time policy tuple drifted");
  auto renamed_time = time.row;
  renamed_time.legacy_fields.canonical_name = "heure";
  renamed_time.legacy_fields.codec_id = "codec_nanos_depuis_minuit";
  Check(dt::IsExactCanonicalTimeTypeCodecIdentityV3(renamed_time),
        "translated time presentation labels changed exact identity");
  const auto reject_time_mutation = [&](auto mutate,
                                        std::string_view message) {
    auto changed = time.row;
    mutate(changed);
    Check(!dt::IsExactCanonicalTimeTypeCodecIdentityV3(changed), message);
  };
  reject_time_mutation([](auto& value) {
      value.legacy_fields.catalog_snapshot_uuid.bytes[15] ^= 1;
    }, "mutated time receipt UUID was admitted");
  reject_time_mutation([](auto& value) {
      --value.legacy_fields.registry_generation;
    }, "mutated time registry generation was admitted");
  reject_time_mutation([](auto& value) {
      value.legacy_fields.canonical_value_exact_bytes = 4;
    }, "mutated time extent was admitted");
  reject_time_mutation([](auto& value) {
      value.hash_policy.uuid.bytes[15] ^= 1;
    }, "mutated time hash policy was admitted");

  Check(timestamp.row.legacy_fields.type_uuid ==
            FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d820") &&
            timestamp.row.legacy_fields.codec_uuid ==
            FixtureUuidLiteral("01a104f5-fb16-7500-93e8-4a99c86d1130") &&
            timestamp.row.legacy_fields.codec_id ==
                "datatype.timestamp.civil_tuple.le.v1" &&
            timestamp.row.legacy_fields.codec_version == 1 &&
            timestamp.row.legacy_fields.codec_generation == 1 &&
            timestamp.row.legacy_fields.canonical_value_minimum_bytes == 16 &&
            timestamp.row.legacy_fields.canonical_value_maximum_bytes == 16 &&
            timestamp.row.legacy_fields.canonical_value_exact_bytes == 16 &&
            timestamp.row.legacy_fields.canonical_binary_type_code == 402 &&
            timestamp.row.legacy_fields.canonical_byte_order == "little_endian" &&
            timestamp.row.legacy_fields.canonical_representation ==
                "i64_local_civil_seconds_u32_nanoseconds_u32_reserved_zero",
        "current timestamp descriptor/type/codec material drifted");
  Check(timestamp.row.descriptor_policy.uuid ==
            FixtureUuidLiteral("01a104ec-8e3c-7dff-8e89-868ecc2a8a0a") &&
            timestamp.row.canonicalization_policy.uuid ==
            FixtureUuidLiteral("01a104ec-8e3c-7dff-8e89-868ecc2a8a0b") &&
            timestamp.row.ordering_policy.uuid ==
            FixtureUuidLiteral("01a104ec-8e3c-7dff-8e89-868ecc2a8a0c") &&
            timestamp.row.hash_policy.uuid ==
            FixtureUuidLiteral("01a104ec-8e3c-7dff-8e89-868ecc2a8a0d") &&
            timestamp.row.operation_policy.uuid ==
            FixtureUuidLiteral("01a104ec-8e3c-7dff-8e89-868ecc2a8a0e") &&
            timestamp.row.descriptor_policy.generation == 1 &&
            timestamp.row.canonicalization_policy.generation == 1 &&
            timestamp.row.ordering_policy.generation == 1 &&
            timestamp.row.hash_policy.generation == 1 &&
            timestamp.row.operation_policy.generation == 1,
        "current timestamp policy tuple drifted");
  Check(historical_timestamp.row.legacy_fields.codec_uuid ==
            FixtureUuidLiteral("01a104f5-fb16-7500-93e8-4a99c86d1130") &&
            historical_timestamp.row.legacy_fields.codec_id ==
                "datatype.timestamp.civil_tuple.le.v1",
        "historical d709 timestamp local-civil identity drifted");
  const auto historical_d708_timestamp = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV8, 8, 8, timestamp_descriptor, 1);
  Check(historical_d708_timestamp.ok &&
            historical_d708_timestamp.row.legacy_fields.codec_uuid ==
                FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d821") &&
            historical_d708_timestamp.row.legacy_fields.codec_id ==
                "datatype.timestamp.utc_tuple.le.v1",
        "historical d708 timestamp UTC identity drifted");
  auto renamed_timestamp = timestamp.row;
  renamed_timestamp.legacy_fields.canonical_name = "horodatage_local";
  renamed_timestamp.legacy_fields.codec_id = "codec_temps_civil_local";
  Check(dt::IsExactCanonicalTimestampTypeCodecIdentityV3(renamed_timestamp),
        "translated timestamp presentation labels changed exact identity");
  const auto reject_timestamp_mutation = [&](auto mutate,
                                             std::string_view message) {
    auto changed = timestamp.row;
    mutate(changed);
    Check(!dt::IsExactCanonicalTimestampTypeCodecIdentityV3(changed), message);
  };
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.catalog_snapshot_uuid.bytes[15] ^= 1;
    }, "mutated timestamp receipt UUID was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.catalog_generation;
    }, "mutated timestamp catalog generation was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.registry_generation;
    }, "mutated timestamp registry generation was admitted");
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.descriptor_uuid.bytes[0] ^= 1;
    }, "mutated timestamp descriptor UUID was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.descriptor_generation;
    }, "mutated timestamp descriptor generation was admitted");
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.type_uuid.bytes[0] ^= 1;
    }, "mutated timestamp type UUID was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.type_generation;
    }, "mutated timestamp type generation was admitted");
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.codec_uuid.bytes[0] ^= 1;
    }, "mutated timestamp codec UUID was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.codec_version;
    }, "mutated timestamp codec version was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.codec_generation;
    }, "mutated timestamp codec generation was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_minimum_bytes;
    }, "mutated timestamp minimum extent was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_maximum_bytes;
    }, "mutated timestamp maximum extent was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_exact_bytes;
    }, "mutated timestamp exact extent was admitted");
  reject_timestamp_mutation([](auto& value) {
      ++value.legacy_fields.canonical_binary_type_code;
    }, "mutated timestamp type code was admitted");
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.canonical_byte_order += ".other";
    }, "mutated timestamp byte order was admitted");
  reject_timestamp_mutation([](auto& value) {
      value.legacy_fields.canonical_representation += ".other";
    }, "mutated timestamp representation was admitted");
  for (const auto member : {
           &dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    auto changed = timestamp.row;
    (changed.*member).uuid.bytes[0] ^= 1;
    Check(!dt::IsExactCanonicalTimestampTypeCodecIdentityV3(changed),
          "mutated timestamp policy UUID was admitted");
    changed = timestamp.row;
    (changed.*member).generation = 2;
    Check(!dt::IsExactCanonicalTimestampTypeCodecIdentityV3(changed),
          "mutated timestamp policy generation was admitted");
  }

  const auto reject_date_mutation = [&](auto mutate,
                                        std::string_view message) {
    auto changed = date.row;
    mutate(changed);
    Check(!dt::IsExactCanonicalDateTypeCodecIdentityV3(changed), message);
  };
  reject_date_mutation([](auto& value) {
      value.legacy_fields.catalog_snapshot_uuid.bytes[15] ^= 1;
    }, "mutated date receipt UUID was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.catalog_generation;
    }, "mutated date catalog generation was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.registry_generation;
    }, "mutated date registry generation was admitted");
  reject_date_mutation([](auto& value) {
      value.legacy_fields.descriptor_uuid.bytes[0] ^= 1;
    }, "mutated date descriptor UUID was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.descriptor_generation;
    }, "mutated date descriptor generation was admitted");
  reject_date_mutation([](auto& value) {
      value.legacy_fields.type_uuid.bytes[0] ^= 1;
    }, "mutated date type UUID was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.type_generation;
    }, "mutated date type generation was admitted");
  reject_date_mutation([](auto& value) {
      value.legacy_fields.codec_uuid.bytes[0] ^= 1;
    }, "mutated date codec UUID was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.codec_version;
    }, "mutated date codec version was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.codec_generation;
    }, "mutated date codec generation was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_minimum_bytes;
    }, "mutated date minimum extent was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_maximum_bytes;
    }, "mutated date maximum extent was admitted");
  reject_date_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_exact_bytes;
    }, "mutated date exact extent was admitted");
  reject_date_mutation([](auto& value) {
      value.legacy_fields.canonical_byte_order += ".other";
    }, "mutated date byte order was admitted");
  reject_date_mutation([](auto& value) {
      value.legacy_fields.canonical_representation += ".other";
    }, "mutated date representation was admitted");
  for (const auto member : {
           &dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    auto changed = date.row;
    (changed.*member).uuid.bytes[0] ^= 1;
    Check(!dt::IsExactCanonicalDateTypeCodecIdentityV3(changed),
          "mutated date policy UUID was admitted");
    changed = date.row;
    (changed.*member).generation = 2;
    Check(!dt::IsExactCanonicalDateTypeCodecIdentityV3(changed),
          "mutated date policy generation was admitted");
  }

  const auto reject_binary_mutation = [&](auto mutate,
                                          std::string_view message) {
    auto changed = binary.row;
    mutate(changed);
    Check(!dt::IsExactCanonicalBinaryTypeCodecIdentityV3(changed), message);
  };
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.catalog_snapshot_uuid.bytes[15] ^= 1;
    }, "mutated binary receipt UUID was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.catalog_generation;
    }, "mutated binary catalog generation was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.registry_generation;
    }, "mutated binary registry generation was admitted");
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.descriptor_uuid.bytes[0] ^= 1;
    }, "mutated binary descriptor UUID was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.descriptor_generation;
    }, "mutated binary descriptor generation was admitted");
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.type_uuid.bytes[0] ^= 1;
    }, "mutated binary type UUID was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.type_generation;
    }, "mutated binary type generation was admitted");
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.codec_uuid.bytes[0] ^= 1;
    }, "mutated binary codec UUID was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.codec_version;
    }, "mutated binary codec version was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.codec_generation;
    }, "mutated binary codec generation was admitted");
  reject_binary_mutation([](auto& value) {
      ++value.legacy_fields.canonical_value_maximum_bytes;
    }, "mutated binary maximum extent was admitted");
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.canonical_byte_order += ".other";
    }, "mutated binary byte order was admitted");
  reject_binary_mutation([](auto& value) {
      value.legacy_fields.canonical_representation += ".other";
    }, "mutated binary representation was admitted");
  for (const auto member : {
           &dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    auto changed = binary.row;
    (changed.*member).uuid.bytes[15] ^= 1;
    Check(!dt::IsExactCanonicalBinaryTypeCodecIdentityV3(changed),
          "mutated binary policy UUID was admitted");
    changed = binary.row;
    (changed.*member).generation = 2;
    Check(!dt::IsExactCanonicalBinaryTypeCodecIdentityV3(changed),
          "mutated binary policy generation was admitted");
  }
  Check(!dt::IsExactCanonicalDateTypeCodecIdentityV3(
            dt::LookupDatatypeTypeCodecIdentityV3(
                dt::kDatatypeCohortV6, 6, 6, date_descriptor, 1).row),
        "historical date row was admitted as current");

  constexpr std::array<std::array<scratchbird::core::platform::Uuid, 2>, 5>
      closures{{
      {FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000"),
       FixtureUuidLiteral("01a1010b-2e50-73c3-bdc8-ca82fc1fae5c")},
      {FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716"),
       FixtureUuidLiteral("01a1010b-2e51-7c10-b90a-af9f08d9cd79")},
      {FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d711"),
       FixtureUuidLiteral("01a1010b-2e52-79a4-8669-a9a7cb89bd21")},
      {FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d714"),
       FixtureUuidLiteral("01a1010b-2e53-7fef-b6fa-1d9300cd10c8")},
      {FixtureUuidLiteral("a0000000-6465-7369-ad61-6c0000000000"),
       FixtureUuidLiteral("01a1010b-2e54-777f-8089-a807f43084c2")},
  }};
  for (const auto& closure : closures) {
    const auto row = dt::LookupDatatypeTypeCodecIdentityV3(
        dt::kDatatypeCohortV8, 8, 8, closure[0], 1);
    Check(row.ok &&
              row.row.legacy_fields.codec_uuid == closure[1],
          "d708 codec-identity closure differs from Core");
  }
}

void TestExactIntervalIdentity() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto descriptor =
      FixtureUuidLiteral("93010000-696e-7465-b276-616c00000000");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, descriptor, 1);
  Check(lookup.ok && dt::IsExactCanonicalIntervalTypeCodecIdentityV3(lookup.row),
        "exact d710 interval identity is absent");
  const auto& row = lookup.row;
  const auto& legacy = row.legacy_fields;
  Check(legacy.type_uuid ==
                FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d822") &&
            legacy.type_generation == 1 &&
            legacy.codec_uuid ==
                FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d823") &&
            legacy.codec_version == 1 && legacy.codec_generation == 1 &&
            legacy.canonical_binary_type_code == 403 &&
            legacy.canonical_value_minimum_bytes == 16 &&
            legacy.canonical_value_maximum_bytes == 16 &&
            legacy.canonical_value_exact_bytes == 16 &&
            !legacy.canonical_value_variable_width && legacy.null_supported &&
            legacy.sql_null_requires_zero_payload &&
            legacy.canonical_byte_order == "little_endian" &&
            legacy.canonical_representation ==
                "i32_months_i32_days_i64_nanoseconds",
        "d710 interval physical identity changed");
  const std::array expected_policies{
      FixtureUuidLiteral("01a10510-696e-7000-8000-000000000001"),
      FixtureUuidLiteral("01a10510-696e-7000-8000-000000000002"),
      FixtureUuidLiteral("01a10510-696e-7000-8000-000000000003"),
      FixtureUuidLiteral("01a10510-696e-7000-8000-000000000004"),
      FixtureUuidLiteral("01a10510-696e-7000-8000-000000000005"),
  };
  const std::array actual_policies{
      row.descriptor_policy, row.canonicalization_policy, row.ordering_policy,
      row.hash_policy, row.operation_policy};
  for (std::size_t index = 0; index < actual_policies.size(); ++index) {
    Check(actual_policies[index].uuid == expected_policies[index] &&
              actual_policies[index].generation == 1,
          "d710 interval semantic policy identity changed");
  }

  const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9, descriptor, 1);
  Check(historical.ok &&
            !dt::IsExactCanonicalIntervalTypeCodecIdentityV3(historical.row) &&
            historical.row.descriptor_policy.uuid.is_nil(),
        "historical d709 interval row acquired current semantic authority");

  auto renamed = row;
  renamed.legacy_fields.canonical_name = "intervalo";
  renamed.legacy_fields.codec_id = "etiqueta_codec_intervalo";
  Check(dt::IsExactCanonicalIntervalTypeCodecIdentityV3(renamed),
        "localized interval labels changed UUID identity");

  auto changed = row;
  changed.legacy_fields.catalog_snapshot_uuid = dt::kDatatypeCohortV9;
  Check(!dt::IsExactCanonicalIntervalTypeCodecIdentityV3(changed),
        "stale interval receipt was admitted as current");
  for (const auto member : {
           &dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
           &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    changed = row;
    (changed.*member).uuid.bytes[15] ^= 1;
    Check(!dt::IsExactCanonicalIntervalTypeCodecIdentityV3(changed),
          "mutated interval policy UUID was admitted");
    changed = row;
    (changed.*member).generation = 2;
    Check(!dt::IsExactCanonicalIntervalTypeCodecIdentityV3(changed),
          "mutated interval policy generation was admitted");
  }
}

void TestExactBlobIdentity() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto descriptor =
      FixtureUuidLiteral("016fd1d3-0daf-5967-b4d7-07fe859a418e");
  const auto type =
      FixtureUuidLiteral("01a1095f-f205-7b29-b679-2ab3755b37d2");
  const auto codec =
      FixtureUuidLiteral("01a1095f-f205-79fe-b1f0-29f1e0f49b05");
  const auto profile =
      FixtureUuidLiteral("01a1095f-f205-739c-8166-221dfe6b818d");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, descriptor, 1);
  Check(lookup.ok && dt::IsExactCanonicalBlobTypeCodecIdentityV3(lookup.row),
        "exact d711 base.blob identity is absent");
  const auto descriptor_identity =
      dt::LookupDatatypeDescriptorIdentityV3(dt::CanonicalTypeId::blob);
  Check(descriptor_identity.ok && descriptor_identity.diagnostic_id.empty() &&
            descriptor_identity.descriptor_uuid.kind ==
                scratchbird::core::platform::UuidKind::object &&
            descriptor_identity.descriptor_uuid.value == descriptor &&
            descriptor_identity.descriptor_generation == 1,
        "datatype descriptor API does not publish exact base.blob UUID identity");
  Check(!dt::LookupDatatypeDescriptorIdentityV3(
             dt::CanonicalTypeId::unknown).ok,
        "descriptor identity API fabricated an unknown-type UUID");
  const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto catalog_blob = catalog.ok()
      ? dt::LookupDatatypeCatalogRow(catalog.manifest,
                                     dt::CanonicalTypeId::blob)
      : dt::DatatypeCatalogManifestResult{};
  Check(catalog.ok() && catalog_blob.ok() &&
            catalog_blob.manifest.descriptor_rows.size() == 1 &&
            catalog_blob.manifest.descriptor_rows.front().descriptor_uuid.kind ==
                scratchbird::core::platform::UuidKind::object &&
            catalog_blob.manifest.descriptor_rows.front().descriptor_uuid.value ==
                descriptor,
        "catalog still selects the synthetic name-derived base.blob UUID");
  const auto& row = lookup.row;
  const auto& legacy = row.legacy_fields;
  Check(legacy.type_uuid == type && legacy.type_generation == 1 &&
            legacy.codec_uuid == codec && legacy.codec_id ==
                "datatype.blob.stream.v1" &&
            legacy.codec_version == 1 && legacy.codec_generation == 1 &&
            legacy.canonical_binary_type_code == 500 &&
            legacy.canonical_value_variable_width && legacy.null_supported &&
            legacy.empty_value_distinct_from_sql_null &&
            legacy.sql_null_requires_zero_payload &&
            legacy.variable_width_storage_without_truncation,
        "base.blob legacy projection surface differs from Core");
  constexpr std::array<Byte, 32> fingerprint{{
      0x5f,0x6d,0xe3,0x00,0x9a,0xce,0x30,0xc9,
      0x70,0x93,0x0b,0x0b,0x26,0x95,0xf4,0x20,
      0x1e,0x78,0x69,0x91,0xfb,0x7b,0x9e,0xf5,
      0xb4,0x95,0x5b,0x04,0x01,0x43,0x5b,0xc8}};
  Check(row.native_fields.present &&
            row.native_fields.canonical_value_minimum_bytes == 0 &&
            row.native_fields.canonical_value_maximum_bytes ==
                static_cast<std::uint64_t>(INT64_MAX) &&
            row.native_fields.canonical_value_transport_width == 0 &&
            row.native_fields.canonical_value_variable_width &&
            row.native_fields.policy_profile_uuid == profile &&
            row.native_fields.policy_profile_generation == 1 &&
            row.native_fields.profile_fingerprint_sha256 == fingerprint,
        "base.blob lossless extent/profile tuple differs from Core");

  const std::array expected_policies{
      FixtureUuidLiteral("01a1095f-f205-7daf-b6f1-291724840de4"),
      FixtureUuidLiteral("01a1095f-f205-7fc9-8fec-e6ada4889d1c"),
      FixtureUuidLiteral("01a1095f-f205-74ae-a0cc-24aa665f5047"),
      FixtureUuidLiteral("01a1095f-f205-77f1-aac4-0e0bd39fe398"),
      FixtureUuidLiteral("01a1095f-f205-7c23-a0d0-91ccd2a078a6"),
  };
  const std::array actual_policies{
      row.descriptor_policy, row.canonicalization_policy, row.ordering_policy,
      row.hash_policy, row.operation_policy};
  for (std::size_t index = 0; index < actual_policies.size(); ++index) {
    Check(actual_policies[index].uuid == expected_policies[index] &&
              actual_policies[index].generation == 1,
          "base.blob five-policy identity differs from Core");
  }

  auto renamed = row;
  renamed.legacy_fields.canonical_name = "objeto_binario";
  renamed.legacy_fields.codec_id = "etiqueta_blob_localizada";
  Check(dt::IsExactCanonicalBlobTypeCodecIdentityV3(renamed),
        "localized base.blob labels changed UUID identity");

  const auto reject = [&](auto mutate, std::string_view message) {
    auto changed = row;
    mutate(changed);
    Check(!dt::IsExactCanonicalBlobTypeCodecIdentityV3(changed), message);
  };
  reject([](auto& value) { --value.native_fields.canonical_value_maximum_bytes; },
         "truncated base.blob u64 maximum was admitted");
  reject([](auto& value) { value.native_fields.present = false; },
         "base.blob without V3-native fields was admitted");
  reject([](auto& value) { value.native_fields.policy_profile_uuid.bytes[0] ^= 1; },
         "mutated base.blob profile UUID was admitted");
  reject([](auto& value) { ++value.native_fields.policy_profile_generation; },
         "mutated base.blob profile generation was admitted");
  reject([](auto& value) { value.native_fields.profile_fingerprint_sha256[31] ^= 1; },
         "mutated base.blob profile fingerprint was admitted");
  reject([](auto& value) { value.legacy_fields.descriptor_uuid.bytes[15] ^= 1; },
         "mutated base.blob descriptor UUID was admitted");
  reject([](auto& value) { value.legacy_fields.type_uuid.bytes[15] ^= 1; },
         "mutated base.blob type UUID was admitted");
  reject([](auto& value) { value.legacy_fields.codec_uuid.bytes[15] ^= 1; },
         "mutated base.blob codec UUID was admitted");
  reject([](auto& value) { value.hash_policy.generation = 2; },
         "mutated base.blob hash policy was admitted");

  const auto projection = dt::ProjectDatatypeTypeCodecIdentityV3ToV1(row);
  Check(!projection.ok &&
            projection.diagnostic_id ==
                "DATATYPE.V1_PROJECTION_UNREPRESENTABLE" &&
            SameLegacyIdentity(projection.row, {}),
        "base.blob was truncated into the frozen V1 identity carrier");
  Check(!dt::LookupDatatypeTypeCodecIdentityV3(
             dt::kDatatypeCohortV10, 10, 10, descriptor, 1).ok,
        "base.blob leaked into the historical d710 cohort");
  Check(!dt::LookupDatatypeTypeCodecIdentityV3(
             dt::kDatatypeCohortV11, 11, 11,
             FixtureUuidLiteral("f4010000-626c-7f62-8000-000000000000"), 1).ok,
        "synthetic name-derived base.blob descriptor was admitted");
}

void TestBlobLegacyRouteIsolation() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto descriptor_uuid =
      FixtureUuidLiteral("016fd1d3-0daf-5967-b4d7-07fe859a418e");

  const auto layout = dt::LookupDatatypeStorageLayout(
      dt::CanonicalTypeId::blob);
  Check(layout.ok() &&
            layout.layout.storage_class ==
                dt::DatatypeStorageClass::logical_large_value &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::blob_component_v3 &&
            layout.layout.inline_bytes == 0 &&
            layout.layout.requires_descriptor &&
            !layout.layout.may_overflow_to_toast,
        "base.blob still publishes a V1/V2 toast or inline layout");

  const auto descriptor =
      dt::LookupDatatypeDescriptor(dt::CanonicalTypeId::blob);
  Check(descriptor.ok(), "base.blob descriptor lookup failed");
  const auto legacy_descriptor =
      dt::SerializeDatatypeDescriptor(descriptor.descriptor);
  Check(!legacy_descriptor.ok() &&
            legacy_descriptor.diagnostic.diagnostic_code ==
                "BLOB.V1_V2_REFUSED",
        "SBDTV001 serialized the current base.blob cohort");

  dt::DatatypeBinaryValue generic_value;
  generic_value.type_id = dt::CanonicalTypeId::blob;
  generic_value.payload = {Byte{0x00}};
  const auto generic_binary = dt::EncodeDatatypeBinaryValue(generic_value);
  Check(!generic_binary.ok() &&
            generic_binary.diagnostic.diagnostic_code ==
                "BLOB.V1_V2_REFUSED",
        "SBDVAL01 encoded the current base.blob cohort");
  const dt::DatatypeBinaryValueView borrowed_blob{
      dt::CanonicalTypeId::blob, false, false,
      generic_value.payload.data(), generic_value.payload.size()};
  const auto structural_binary =
      dt::ValidateDatatypeBinaryStructuralValueViewNoAlloc(borrowed_blob);
  Check(!structural_binary.ok() &&
            structural_binary.diagnostic.diagnostic_code ==
                "BLOB.V1_V2_REFUSED",
        "structural SBDVAL01 admitted the current base.blob cohort");

  dt::DatatypePhysicalValue generic_physical;
  generic_physical.type_id = dt::CanonicalTypeId::blob;
  generic_physical.state = dt::DatatypePhysicalValueState::value;
  generic_physical.payload = {Byte{0x00}};
  const auto physical =
      dt::EncodeDatatypePhysicalValue(generic_physical);
  Check(!physical.ok() &&
            physical.diagnostic.diagnostic_code ==
                "BLOB.V1_V2_REFUSED",
        "SBDPV001 encoded the current base.blob cohort");
  const dt::DatatypePhysicalValueView borrowed_physical{
      dt::CanonicalTypeId::blob, dt::DatatypePhysicalValueState::value,
      generic_physical.payload.data(), generic_physical.payload.size()};
  const auto structural_physical =
      dt::ValidateDatatypePhysicalStructuralValueViewNoAlloc(
          borrowed_physical);
  Check(!structural_physical.ok() &&
            structural_physical.diagnostic.diagnostic_code ==
                "BLOB.V1_V2_REFUSED",
        "structural SBDPV001 admitted the current base.blob cohort");

  const auto legacy_wire =
      dt::WireTypeIdForCanonicalTypeId(dt::CanonicalTypeId::blob);
  Check(legacy_wire.type_family == 0 && legacy_wire.type_code == 0 &&
            legacy_wire.type_version == 0 && legacy_wire.type_flags == 0,
        "base.blob still maps to the numeric V1 native-wire LOB family");
  Check(dt::CanonicalTypeIdFromWireTypeId(legacy_wire) ==
            dt::CanonicalTypeId::unknown,
        "the unallocated all-zero native-wire identity inferred base.blob");
  dt::CanonicalWireTypeId old_lob_wire;
  old_lob_wire.type_family =
      static_cast<std::uint16_t>(dt::CanonicalWireTypeFamily::lob);
  old_lob_wire.type_code = 1;
  old_lob_wire.type_version = 1;
  old_lob_wire.type_flags = dt::CanonicalWireTypeFlagBit(
      dt::CanonicalWireTypeFlag::lob_locator_metadata_required);
  Check(dt::CanonicalTypeIdFromWireTypeId(old_lob_wire) ==
            dt::CanonicalTypeId::unknown,
        "numeric V1 native-wire LOB metadata inferred base.blob identity");

  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::blob,
                                 dt::CanonicalTypeId::blob, false) ==
            dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::binary,
                                     dt::CanonicalTypeId::blob, false) ==
                dt::DatatypeCastCategory::forbidden,
        "enum-only generic casts admitted the current base.blob cohort");

  dt::DatatypeStorageIdentityV3 storage_v3;
  Check(dt::LookupDatatypeStorageIdentityV3(
            dt::kDatatypeCohortV11, 11, 11, descriptor_uuid, 1,
            &storage_v3) &&
            storage_v3.type_id == dt::CanonicalTypeId::blob &&
            storage_v3.codec.has_value() &&
            dt::IsExactCanonicalBlobTypeCodecIdentityV3(*storage_v3.codec),
        "V3 storage identity did not preserve exact base.blob authority");
  dt::DatatypeStorageIdentityV1 storage_v1;
  Check(!dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV11, 11, 11, descriptor_uuid, 1,
            &storage_v1),
        "base.blob downgraded into the historical V1 storage identity");
}

void TestExactProfileCohortsRemainIndependent() {
  using scratchbird::tests::FixtureUuidLiteral;
  using Predicate = bool (*)(const dt::DatatypeTypeCodecIdentityRowV3&) noexcept;
  struct Case { scratchbird::core::platform::Uuid descriptor; Predicate exact; };
  const Case cases[] = {
      {FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000"), dt::IsExactCanonicalBinaryTypeCodecIdentityV3},
      {FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829"), dt::IsExactCanonicalBitStringTypeCodecIdentityV3},
      {FixtureUuidLiteral("90010000-6461-7465-8000-000000000000"), dt::IsExactCanonicalDateTypeCodecIdentityV3},
      {FixtureUuidLiteral("91010000-7469-7d65-8000-000000000000"), dt::IsExactCanonicalTimeTypeCodecIdentityV3},
      {FixtureUuidLiteral("92010000-7469-7d65-b374-616d70000000"), dt::IsExactCanonicalTimestampTypeCodecIdentityV3},
      {FixtureUuidLiteral("93010000-696e-7465-b276-616c00000000"), dt::IsExactCanonicalIntervalTypeCodecIdentityV3}};
  for (const auto& test : cases) {
    const auto descriptor = test.descriptor;
    const auto old = dt::LookupDatatypeTypeCodecIdentityV3(
        dt::kDatatypeCohortV10, 10, 10, descriptor, 1);
    const auto next = dt::LookupDatatypeTypeCodecIdentityV3(
        dt::kDatatypeCohortV11, 11, 11, descriptor, 1);
    Check(old.ok && next.ok && test.exact(old.row) && test.exact(next.row),
          "exact retained/current profile identity rejected");
    const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
        dt::kDatatypeCohortV9, 9, 9, descriptor, 1);
    Check(historical.ok && !test.exact(historical.row),
          "older profile admitted without an owning profile contract");
    for (const auto* original : {&old.row, &next.row}) {
      auto changed = *original;
      changed.legacy_fields.catalog_snapshot_uuid =
          original == &old.row ? dt::kDatatypeCohortV11 : dt::kDatatypeCohortV10;
      Check(!test.exact(changed), "mixed snapshot admitted");
      changed = *original;
      ++changed.legacy_fields.catalog_generation;
      Check(!test.exact(changed), "mixed catalog generation admitted");
      changed = *original;
      ++changed.legacy_fields.registry_generation;
      Check(!test.exact(changed), "mixed registry generation admitted");
      changed = *original;
      ++changed.legacy_fields.descriptor_generation;
      Check(!test.exact(changed), "unregistered descriptor generation admitted");
      changed = *original;
      ++changed.legacy_fields.codec_generation;
      Check(!test.exact(changed), "unregistered codec generation admitted");
      for (auto member : {&dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
                          &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
                          &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
                          &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
                          &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
        changed = *original;
        ++(changed.*member).generation;
        Check(!test.exact(changed), "unregistered policy generation admitted");
        changed = *original;
        (changed.*member).uuid.bytes[0] ^= 1;
        Check(!test.exact(changed), "unregistered binary policy identity admitted");
      }
      changed = *original;
      changed.native_fields = original == &old.row ? next.row.native_fields
                                                  : old.row.native_fields;
      Check(!test.exact(changed), "native fields laundered across cohorts");
      changed = *original;
      changed.legacy_fields.canonical_name = "localized display only";
      changed.legacy_fields.codec_id = "localized codec label";
      Check(test.exact(changed), "display labels treated as identity");
    }
  }
}

void TestConformanceExamplesBindExactReceipts() {
  const auto snapshot = dt::kDatatypeCohortV10;
  const auto loaded = dt::LoadCurrentCoreDatatypeConformanceManifest(
      {snapshot, snapshot, 10, 10}, false,
      {snapshot, snapshot, 10, 10}, false,
      {snapshot, snapshot, 10, 10}, false,
      {snapshot, snapshot, 10, 10}, false,
      {snapshot, snapshot, 10, 10}, false);
  // Isolate the five typed routes: other manifest coverage/diagnostics are
  // qualified separately by the complete-manifest gate, not suppressed here.
  const auto test = [&](auto examples, auto executed, std::string_view message) {
    Check((loaded.manifest.*examples).size() == 1,
          "exact receipt failed to load typed conformance example");
    dt::DatatypeConformanceManifest isolated;
    isolated.manifest_key = dt::kCurrentCoreDatatypeConformanceManifestKey;
    isolated.inventory_source_path = "project/src/core/datatypes/datatype_descriptor.cpp";
    isolated.*examples = loaded.manifest.*examples;
    const auto valid = dt::ExecuteDatatypeConformanceManifest(isolated);
    Check(valid.*executed == 1, "exact typed example did not execute");
    auto& example = (isolated.*examples).front();
    const auto successor = dt::LookupDatatypeTypeCodecIdentityV3(
        dt::kDatatypeCohortV11, 11, 11,
        example.identity.legacy_fields.descriptor_uuid, 1);
    Check(successor.ok, "missing successor fixture");
    example.identity = successor.row;
    const auto refused = dt::ExecuteDatatypeConformanceManifest(isolated);
    Check(refused.*executed == 0, "mixed-cohort evidence was executed");
    bool exact_diagnostic = false;
    for (const auto& diagnostic : refused.diagnostics)
      if (diagnostic.message_key == message) exact_diagnostic = true;
    Check(exact_diagnostic, "mixed-cohort evidence lost exact identity diagnostic");
  };
  test(&dt::DatatypeConformanceManifest::bit_string_examples,
       &dt::DatatypeConformanceManifestResult::executed_bit_string_examples,
       "datatype.conformance.bit_string_identity_refused");
  test(&dt::DatatypeConformanceManifest::date_examples,
       &dt::DatatypeConformanceManifestResult::executed_date_examples,
       "datatype.conformance.date_identity_refused");
  test(&dt::DatatypeConformanceManifest::time_examples,
       &dt::DatatypeConformanceManifestResult::executed_time_examples,
       "datatype.conformance.time_identity_refused");
  test(&dt::DatatypeConformanceManifest::timestamp_examples,
       &dt::DatatypeConformanceManifestResult::executed_timestamp_examples,
       "datatype.conformance.timestamp_identity_refused");
  test(&dt::DatatypeConformanceManifest::interval_examples,
       &dt::DatatypeConformanceManifestResult::executed_interval_examples,
       "datatype.conformance.interval_identity_refused");
}

void TestLookupAllocationFailureIsContained() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto date_descriptor =
      FixtureUuidLiteral("90010000-6461-7465-8000-000000000000");

  allocation_probe::fail_next = true;
  const auto invalid = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10, 9, 10, date_descriptor, 1);
  const bool invalid_path_did_not_allocate = allocation_probe::fail_next;
  allocation_probe::fail_next = false;
  Check(invalid_path_did_not_allocate && !invalid.ok &&
            invalid.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
        "invalid identity lookup allocated its diagnostic");

  allocation_probe::fail_next = true;
  const auto refused = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, date_descriptor, 1);
  Check(!allocation_probe::fail_next,
        "identity row copy did not exercise the allocation-failure probe");
  Check(!refused.ok &&
            refused.diagnostic_id == "RESOURCE.BUDGET_EXCEEDED" &&
            SameV3Identity(refused.row, {}),
        "identity row allocation failure escaped or lost its admitted diagnostic");

  const auto recovered = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV11, 11, 11, date_descriptor, 1);
  Check(recovered.ok && recovered.diagnostic_id.empty() &&
            dt::IsExactCanonicalDateTypeCodecIdentityV3(recovered.row),
        "identity lookup did not recover after an injected allocation failure");

  const auto projection_source = recovered.row;
  const dt::DatatypeTypeCodecIdentityRowV1 default_projection_row;
  allocation_probe::fail_next = true;
  const auto projection_refused =
      dt::ProjectDatatypeTypeCodecIdentityV3ToV1(projection_source);
  Check(!allocation_probe::fail_next,
        "V3-to-V1 projection did not exercise the allocation-failure probe");
  Check(!projection_refused.ok &&
            projection_refused.diagnostic_id == "RESOURCE.BUDGET_EXCEEDED" &&
            SameLegacyIdentity(projection_refused.row, default_projection_row),
        "V3-to-V1 projection failure escaped, published a partial row, or lost its diagnostic");

  const auto projection_recovered =
      dt::ProjectDatatypeTypeCodecIdentityV3ToV1(projection_source);
  Check(projection_recovered.ok && projection_recovered.diagnostic_id.empty() &&
            SameLegacyIdentity(projection_recovered.row,
                               projection_source.legacy_fields),
        "V3-to-V1 projection did not recover with exact legacy fields");
}

}  // namespace

int main() {
  TestCoreD708CanonicalJsonAndCompiledRows();
  TestPopulationAndAuthoritativeRows();
  TestExactBitStringIdentity();
  TestExactCurrentBinaryDateAndCodecClosures();
  TestExactIntervalIdentity();
  TestExactBlobIdentity();
  TestBlobLegacyRouteIsolation();
  TestExactProfileCohortsRemainIndependent();
  TestConformanceExamplesBindExactReceipts();
  TestLookupAllocationFailureIsContained();
  std::cout << "datatype_identity_v3_test=passed\n";
  return EXIT_SUCCESS;
}
