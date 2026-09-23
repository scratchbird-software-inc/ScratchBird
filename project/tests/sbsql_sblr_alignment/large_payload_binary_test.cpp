// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/storage/page/large_payload.hpp"
#include "../../src/storage/page/hot_cold_row_split.hpp"
#include "../../src/storage/page/payload_binary_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace p = scratchbird::storage::page;
namespace c = scratchbird::core::platform;
static void Check(bool value, std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static c::TypedUuid Id(c::UuidKind kind,unsigned n) {
  c::TypedUuid id;id.kind=kind;
  id.value.bytes={1,144,10,9,0,124,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;
}
int main() {
  p::LargePayloadDescriptor d;
  d.payload_uuid=Id(c::UuidKind::object,1);d.owner_object_uuid=Id(c::UuidKind::object,2);
  d.generation_scope_uuid=Id(c::UuidKind::row,3);d.filespace_uuid=Id(c::UuidKind::filespace,4);
  d.overflow_value_uuid=Id(c::UuidKind::object,5);d.generation=1;d.byte_count=9;
  d.filespace_class="large_blob";d.page_family="blob";d.content_hash=std::string("x;=\0y",5);
  auto bytes=p::SerializeLargePayloadDescriptor(d);Check(bytes.starts_with("SBLPD002"));
  auto decoded=p::ParseLargePayloadDescriptor(bytes);Check(decoded.has_value());
  Check(decoded->payload_uuid.value==d.payload_uuid.value&&decoded->generation_scope_uuid.kind==c::UuidKind::row);
  Check(decoded->generation_scope_uuid.value==d.generation_scope_uuid.value&&decoded->content_hash==d.content_hash);
  Check(p::SerializeLargePayloadDescriptor(*decoded)==bytes);
  for(std::size_t n=0;n<bytes.size();++n)Check(!p::ParseLargePayloadDescriptor(bytes.substr(0,n)));
  Check(!p::ParseLargePayloadDescriptor(bytes+"x"));
  Check(!p::ParseLargePayloadDescriptor("SB_LARGE_PAYLOAD_DESCRIPTOR_V1;payload_uuid=01900a09-007c-7000-8000-000000000001"));
  auto corrupt=bytes;corrupt[8]=127;Check(!p::ParseLargePayloadDescriptor(corrupt));
  corrupt=bytes;corrupt[9+6]=0;Check(!p::ParseLargePayloadDescriptor(corrupt));
  d.inline_payload=true;d.inline_text=std::string("a;=\0z",5);d.byte_count=5;
  bytes=p::SerializeLargePayloadDescriptor(d);decoded=p::ParseLargePayloadDescriptor(bytes);
  Check(decoded&&decoded->inline_text==d.inline_text);
  p::HotColdRowHead head;head.row_uuid=Id(c::UuidKind::row,6);head.owner_object_uuid=d.owner_object_uuid;
  head.transaction_uuid=Id(c::UuidKind::transaction,7);head.creator_local_transaction_id=8;head.row_version=9;
  head.hot_filespace_class="hot_row";head.cold_row_filespace_class="cold_row";
  head.hot_fields.push_back({"id",std::string(reinterpret_cast<const char*>(head.row_uuid.value.bytes.data()),16)});
  bytes=p::SerializeHotColdRowHead(head);Check(bytes.starts_with("SBHCR002"));
  p::payload_binary::Reader reader{bytes,8};c::TypedUuid id;
  Check(reader.Uuid(id)&&id.value==head.row_uuid.value);
  Check(reader.Uuid(id)&&id.value==head.owner_object_uuid.value);
  Check(reader.Uuid(id)&&id.value==head.transaction_uuid.value);
  std::uint64_t number=0;std::string text;
  Check(reader.U64(number)&&number==8);Check(reader.U64(number)&&number==9);
  Check(reader.String(text)&&text=="hot_row");Check(reader.String(text)&&text=="cold_row");
  Check(reader.U64(number)&&number==1);Check(reader.String(text)&&text=="id");
  Check(reader.String(text)&&text.size()==16);Check(reader.U64(number)&&number==0);
  Check(reader.cursor==bytes.size());
}
