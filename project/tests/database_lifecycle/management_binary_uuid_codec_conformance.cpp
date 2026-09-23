// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "management_request_codec.hpp"
#include <cstdlib>
#include <iostream>
using namespace scratchbird;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "management codec check failed at " << __LINE__ << '\n'; std::abort(); } } while (false)
int main() {
  wire::ManagementRequestV1 request;
  request.operation_key = "status_database";
  request.target_uuid.bytes = {1,0,10,13,124,9,0x70,0,0x80,0,58,59,44,61,0,1};
  request.audit_reason = "binary identity probe";
  request.timeout_ms = UINT64_MAX;
  request.include_history = true;
  std::vector<std::uint8_t> bytes;
  CHECK(wire::EncodeManagementRequestV1(request, &bytes));
  wire::ManagementRequestV1 decoded;
  CHECK(wire::DecodeManagementRequestV1(bytes, &decoded));
  CHECK(decoded.target_uuid == request.target_uuid && decoded.timeout_ms == UINT64_MAX && decoded.include_history);
  std::string_view raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  auto position = raw.find("target_uuid");
  CHECK(position != std::string_view::npos);
  position += 11;
  CHECK(bytes[position] == 16 && bytes[position+1] == 0);
  CHECK(std::equal(request.target_uuid.bytes.begin(), request.target_uuid.bytes.end(), bytes.begin()+position+2));
  auto malformed = bytes;
  malformed.push_back(0);
  CHECK(!wire::DecodeManagementRequestV1(malformed, &decoded));
  for (std::size_t n=0; n<bytes.size(); ++n)
    CHECK(!wire::DecodeManagementRequestV1(std::span(bytes.data(), n), &decoded));
  const auto replace_target = [&](const std::string& value) {
    auto packet = bytes;
    packet.erase(packet.begin()+position+2, packet.begin()+position+18);
    packet.insert(packet.begin()+position+2,value.begin(),value.end());
    packet[position] = value.size(); packet[position+1] = value.size() >> 8;
    return packet;
  };
  for (const auto& value : {std::string(15,'x'), std::string(17,'x'), std::string("019f0900-0000-7000-8000-000000002b01")})
    CHECK(!wire::DecodeManagementRequestV1(replace_target(value), &decoded));
  malformed=bytes;malformed[position+2+6]=0x60;
  CHECK(!wire::DecodeManagementRequestV1(malformed,&decoded));
  malformed=bytes;malformed[position+2+8]=0;
  CHECK(!wire::DecodeManagementRequestV1(malformed,&decoded));
  // Same-length unknown and duplicate keys must not overwrite fields.
  auto replace_key = [&](std::string_view key,std::string_view value) {
    auto packet=bytes;
    std::string_view source(reinterpret_cast<const char*>(packet.data()),packet.size());
    const auto at=source.find(key);CHECK(at!=std::string_view::npos && key.size()==value.size());
    std::copy(value.begin(),value.end(),packet.begin()+at);return packet;
  };
  CHECK(!wire::DecodeManagementRequestV1(replace_key("audit_reason","include_hist"),&decoded));
  // Rebuild a duplicate operation_key by changing the 11-byte target key extent and bytes.
  malformed=bytes;
  const auto key_start=position-11;
  malformed.erase(malformed.begin()+key_start,malformed.begin()+key_start+11);
  std::string duplicate="operation_key";
  malformed.insert(malformed.begin()+key_start,duplicate.begin(),duplicate.end());
  malformed[key_start-2]=duplicate.size();
  CHECK(!wire::DecodeManagementRequestV1(malformed,&decoded));
  CHECK(!wire::DecodeManagementRequestV1(replace_key("true","oops"),&decoded));
  const auto identity_bytes = wire::ManagementTargetBytes(request.target_uuid);
  request.mode = "expected_database_uuid:" + identity_bytes + ";allow_mutation:true";
  CHECK(wire::EncodeManagementRequestV1(request,&malformed));
  CHECK(wire::DecodeManagementRequestV1(malformed,&decoded) && decoded.mode == request.mode);
  std::vector<std::string> mode_fields;
  CHECK(wire::SplitManagementMode(request.mode, &mode_fields) && mode_fields.size() == 2 &&
        mode_fields[0] == "expected_database_uuid:" + identity_bytes && mode_fields[1] == "allow_mutation:true");
  CHECK(!wire::SplitManagementMode("expected_database_uuid:019f0900-0000-7000-8000-000000002b01", &mode_fields));
  CHECK(!wire::SplitManagementMode(request.mode + ";allow_mutation:false", &mode_fields));
  CHECK(!wire::SplitManagementMode("expected_database_uuid:" + identity_bytes + "extra;allow_mutation:true", &mode_fields));
  request.mode.clear();
  request.target_uuid={};request.timeout_ms=0;
  CHECK(wire::EncodeManagementRequestV1(request,&bytes) && wire::DecodeManagementRequestV1(bytes,&decoded) && decoded.target_uuid.is_nil());
  const auto prior=bytes;request.mode.assign(65535,'x');
  CHECK(!wire::EncodeManagementRequestV1(request,&bytes) && bytes==prior);
  std::cout << "management codec binary16 roundtrip/malformed gates passed\n";
}
