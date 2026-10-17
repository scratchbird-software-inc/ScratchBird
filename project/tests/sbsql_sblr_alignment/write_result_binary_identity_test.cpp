// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/write_result_policy.hpp"
#include "dml/import_reject_model.hpp"
#include "../../src/engine/public_abi_int64_payload.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace api = scratchbird::engine::internal_api;
static void Check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(EXIT_FAILURE); }
}

int main() {
  for(bool references:{false,true}) {
    const auto schema=api::BuildDefaultImportErrorRowSchema(references);
    Check(schema.columns.size()==(references?13:11),"default import reject schema shape changed");
    Check(schema.columns[0].descriptor.canonical_type_name=="uint64"&&
        schema.columns[2].descriptor.canonical_type_name=="uuid"&&!schema.columns[2].nullable,
        "reject row number or target UUID schema is not its actual native datatype");
    if(references)Check(schema.columns[11].descriptor.canonical_type_name=="uuid"&&schema.columns[11].nullable,
        "optional reject payload identity is incorrectly declared TEXT");
  }
  for(const std::uint64_t number:{std::uint64_t{0},std::uint64_t{1},std::uint64_t{1}<<63,UINT64_MAX}) {
    api::EngineApiResult result;
    result.dml_summary.rows_changed=number;result.dml_summary.visible_rows_scanned=number;
    result.dml_summary.index_probes=number;result.dml_summary.append_calls=number;
    result.dml_summary.file_opens=number;result.dml_summary.flushes=number;
    result.dml_summary.page_reservations=number;
    const auto policy=api::ResolveWriteResultPolicyOptions({"write_result_policy=summary_only"},"dml.insert_rows");
    api::ApplyWriteResultPolicy(policy,&result);
    Check(result.result_shape.rows.size()==1,"summary row absent");
    unsigned counters=0;
    for(const auto& [name,value]:result.result_shape.rows.front().fields) {
      std::string_view payload;
      Check(scratchbird::engine::PublicUInt64ScalarPayloadV1(value,&payload)&&payload.size()==8,
          "summary UINT64 is not an exact native machine/wire carrier");
      const auto expected=name=="result_rows_before_policy"?std::uint64_t{0}:number;
      for(unsigned i=0;i<8;++i)Check(static_cast<unsigned char>(payload[i])==((expected>>(8*i))&255),
          "summary UINT64 value/endian bits changed");
      ++counters;
    }
    Check(counters==8,"summary counter set changed");
  }
  {
    api::EngineTypedValue value;value.descriptor.canonical_type_name="uint64";value.binary_value.assign(8,0xff);
    std::string_view output="unchanged";
    Check(scratchbird::engine::PublicUInt64ScalarPayloadV1(value,&output)&&output.size()==8,"native maximum UINT64 refused");
    for(unsigned mutation=0;mutation<7;++mutation) {
      auto invalid=value;
      if(mutation==0)invalid.binary_value.clear();
      if(mutation==1)invalid.binary_value.pop_back();
      if(mutation==2)invalid.binary_value.push_back(0);
      if(mutation==3)invalid.encoded_value="18446744073709551615";
      if(mutation==4)invalid.setState(api::EngineValueState::missing);
      if(mutation==5)invalid.is_null=true;
      if(mutation==6)invalid.descriptor.canonical_type_name="int64";
      output="unchanged";
      Check(!scratchbird::engine::PublicUInt64ScalarPayloadV1(invalid,&output)&&output=="unchanged",
          "invalid UINT64 carrier accepted or failure changed borrowed output");
    }
    value.setState(api::EngineValueState::sql_null);value.binary_value.clear();
    Check(scratchbird::engine::PublicUInt64ScalarPayloadV1(value,&output)&&output.empty(),"clean native NULL refused");
    value.encoded_value="0";
    Check(!scratchbird::engine::PublicUInt64ScalarPayloadV1(value,&output),"dirty UINT64 NULL accepted");
  }
  const auto original = scratchbird::tests::FixtureUuid(48, 1);
  for (const char* policy_name : {"ids_only", "changed_fields"}) {
    const auto policy = api::ResolveWriteResultPolicyOptions(
        {std::string("write_result_policy:") + policy_name}, "dml.insert_rows");
    Check(policy.ok, "valid result policy rejected");
    for (unsigned bit = 0; bit <= 128; ++bit) {
      auto id = original;
      if (bit < 128) id.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
      api::EngineApiResult result;
      api::EngineRowValue row;
      row.requested_row_uuid = id;
      api::EngineTypedValue text;
      text.descriptor.canonical_type_name = "text";
      text.encoded_value = "019d0000-0000-7000-8000-000000000001";
      row.fields.push_back({"customer_uuid", text});
      result.result_shape.rows.push_back(row);
      api::ApplyWriteResultPolicy(policy, &result);
      Check(result.result_shape.rows.size() == 1, "result row lost");
      const auto& actual = result.result_shape.rows.front();
      Check(actual.requested_row_uuid == id, "requested UUID changed");
      bool found = false, kept_text = false;
      for (const auto& [name, value] : actual.fields) {
        if (name == "row_uuid") {
          found = true;
          Check(value.descriptor.canonical_type_name == "uuid" &&
                    value.binary_value == std::vector<std::uint8_t>(id.bytes.begin(), id.bytes.end()) &&
                    value.encoded_value.empty() && !value.is_null,
                "result identity was formatted as text or lost UUID bits");
        } else if (name == "changed_field_count") {
          std::string_view payload;
          Check(scratchbird::engine::PublicUInt64ScalarPayloadV1(value,&payload)&&payload.size()==8&&
              static_cast<unsigned char>(payload[0])==1,"changed-field count is not native UINT64 one");
          for(unsigned i=1;i<8;++i)Check(payload[i]==0,"changed-field count high bits changed");
        } else if (name == "customer_uuid" || name == "changed.customer_uuid") {
          kept_text = true;
          Check(value.descriptor.canonical_type_name == "text" &&
                    value.encoded_value == text.encoded_value && value.binary_value.empty(),
                "user TEXT was reinterpreted as identity");
        }
      }
      Check(found && kept_text, "identity or existing typed field missing");
    }
  }
  std::puts("write-result binary identity: PASS; raw16 preserved under both policies");
}
