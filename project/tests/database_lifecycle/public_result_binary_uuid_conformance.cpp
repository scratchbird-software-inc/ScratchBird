#include "../../drivers/tool/cli/binary_status_display.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/public_result_packet.hpp"
#include "../support/client_public_result_display.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace packet = scratchbird::wire::public_result;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at " << __LINE__ << '\n'; std::exit(EXIT_FAILURE); } } while (false)
int main() {
  const std::string uuid("\x01\x9f\x00\x0a\x3b\x7c\x70\x00\x80\x00\x3d\x3a\xff\x00\x00\x01", 16);
  const std::string uuid_text = "019f000a-3b7c-7000-8000-3d3aff000001";
  std::vector<packet::Field> fields = {
    {"transaction_uuid", packet::Kind::uuid, uuid},
    {"user_text", packet::Kind::text, uuid_text},
    {"count", packet::Kind::unsigned_integer, packet::Unsigned(std::numeric_limits<std::uint64_t>::max())}};
  std::string encoded;
  CHECK(packet::Encode(fields, &encoded));
  std::vector<packet::Field> decoded;
  CHECK(packet::Decode(encoded, &decoded));
  CHECK(decoded.size() == fields.size());
  for (std::size_t i=0; i<fields.size(); ++i) {
    CHECK(decoded[i].name == fields[i].name);
    CHECK(decoded[i].kind == fields[i].kind);
    CHECK(decoded[i].value == fields[i].value);
  }
  CHECK(packet::Find(encoded, "transaction_uuid")->kind == packet::Kind::uuid);
  CHECK(packet::Find(encoded, "user_text")->kind == packet::Kind::text);
  CHECK(packet::AsUnsigned(decoded[2]) == std::numeric_limits<std::uint64_t>::max());
  CHECK(!packet::AsUnsigned(decoded[0]));
  for (std::size_t size=0; size<encoded.size(); ++size) {
    decoded = {{"unchanged", packet::Kind::text, "sentinel"}};
    CHECK(!packet::Decode(std::string_view(encoded).substr(0,size), &decoded));
    CHECK(decoded.size() == 1 && decoded[0].value == "sentinel");
  }
  CHECK(!packet::Decode(encoded + "x", &decoded));
  for (const auto& invalid : {std::string(15,'x'), std::string(17,'x'), uuid_text}) {
    std::string unchanged = "sentinel";
    CHECK(!packet::Encode(std::vector<packet::Field>{{"uuid", packet::Kind::uuid, invalid}}, &unchanged));
    CHECK(unchanged == "sentinel");
  }
  fields.push_back({"transaction_uuid", packet::Kind::uuid, std::string(16,'\0')});
  CHECK(packet::Encode(fields, &encoded));
  CHECK(!packet::Find(encoded, "transaction_uuid"));
  CHECK(packet::Find(encoded, "count"));
  auto malformed = encoded;
  // First field kind follows magic, count, name length and name bytes.
  malformed[8+4+4+fields[0].name.size()] = '\xff';
  CHECK(!packet::Decode(malformed, &decoded));
  malformed = encoded;
  for (unsigned i=0; i<8; ++i) malformed[8+4+4+fields[0].name.size()+1+i] = '\xff';
  CHECK(!packet::Decode(malformed, &decoded));
  std::vector<packet::Field> row = {{"object_uuid", packet::Kind::uuid, uuid}, {"name", packet::Kind::text, std::string("a\0;b\nc",6)}};
  std::string row_packet;
  CHECK(packet::Encode(row, &row_packet));
  CHECK(packet::Encode(std::vector<packet::Field>{{"row[0]", packet::Kind::row, row_packet}}, &encoded));
  const auto found_row = packet::Find(encoded, "row[0]");
  CHECK(found_row && found_row->kind == packet::Kind::row);
  CHECK(packet::Find(found_row->value, "object_uuid")->value == uuid);
  std::string evidence;
  CHECK(packet::Encode(std::vector<packet::Field>{{"transaction", packet::Kind::uuid, uuid}}, &evidence));
  std::vector<packet::Field> evidence_fields = {{"evidence", packet::Kind::evidence, evidence}};
  CHECK(packet::Encode(evidence_fields, &encoded));
  CHECK(packet::Evidence(encoded, "transaction")->value == uuid);
  evidence_fields.push_back(evidence_fields.front());
  CHECK(packet::Encode(evidence_fields, &encoded));
  CHECK(!packet::Evidence(encoded, "transaction"));
  namespace status = scratchbird::wire::binary_status;
  status::Stream nested;
  nested << "{\"uuid\":\"" << status::Identity(std::string_view(uuid)) << "\"}";
  const auto nested_bytes=nested.str();
  CHECK(nested_bytes.find(uuid)!=std::string::npos);
  CHECK(nested_bytes.find(uuid_text)==std::string::npos);
  status::Stream parent;
  parent << "[" << nested_bytes << "]";
  const auto status_bytes=parent.str();
  CHECK(scratchbird::cli::RenderBinaryStatus(status_bytes)=="[{\"uuid\":\""+uuid_text+"\"}]");
  for (std::size_t n=0;n<status_bytes.size();++n)
    CHECK(!scratchbird::cli::RenderBinaryStatus(std::string_view(status_bytes).substr(0,n)));
  for (const auto& invalid : {std::string(15,'x'),std::string(17,'x'),uuid_text}) {
    bool refused=false;
    try { (void)status::Identity(std::string_view(invalid)); } catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
  }
  // Exercise the process CLIENT renderer with delimiter-bearing UUID bytes,
  // nested rows, full-width numbers and malformed framing at every boundary.
  const auto display = scratchbird::tests::DisplayPublicResultPacket(row_packet);
  CHECK(display.find("object_uuid=" + uuid_text) != std::string::npos);
  CHECK(display.find(uuid) == std::string::npos);
  const auto rejects_display = [](std::string_view bytes) {
    try { (void)scratchbird::tests::DisplayPublicResultPacket(bytes); }
    catch (const std::invalid_argument&) { return true; }
    return false;
  };
  for (std::size_t n=0; n<row_packet.size(); ++n)
    CHECK(rejects_display(std::string_view(row_packet).substr(0,n)));
  CHECK(rejects_display(row_packet + "x"));
  CHECK(rejects_display(malformed));
  CHECK(packet::Encode(std::vector<packet::Field>{
      {"row[0]", packet::Kind::row, row_packet},
      {"count", packet::Kind::unsigned_integer, packet::Unsigned(UINT64_MAX)}}, &encoded));
  const auto row_display = scratchbird::tests::DisplayPublicResultPacket(encoded);
  CHECK(row_display.find("row[0]=object_uuid=" + uuid_text + ";name=") == 0);
  CHECK(row_display.find("count=18446744073709551615") != std::string::npos);
  CHECK(packet::Encode(std::vector<packet::Field>{
      {"row[0]", packet::Kind::row, row_packet + "x"}}, &encoded));
  CHECK(rejects_display(encoded));
  auto deeply_nested = row_packet;
  for (unsigned i=0; i<3; ++i) {
    CHECK(packet::Encode(std::vector<packet::Field>{
        {"row[0]", packet::Kind::row, deeply_nested}}, &encoded));
    deeply_nested = encoded;
  }
  CHECK(rejects_display(deeply_nested));
  std::cout << "public result binary UUID codec: PASS\n";
}
