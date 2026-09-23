// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/artifacts/artifact_api.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value, std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id; id.bytes={1,144,10,9,0,124,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};
  return id;
}
static a::EngineTypedValue Uuid(const a::EngineUuid& id) {
  a::EngineTypedValue value; value.binary_value.assign(id.bytes.begin(),id.bytes.end());return value;
}
int main() {
  a::EngineRowValue row;
  row.fields={{"object_uuid",Uuid(Id(1))},{"target_schema_uuid",Uuid(Id(2))}};
  Check(a::ArtifactIdentityFieldsValid(row));
  Check(a::FieldUuid(row,"object_uuid")==Id(1));
  auto bad=row;bad.fields[0].second.binary_value.pop_back();
  Check(!a::ArtifactIdentityFieldsValid(bad));
  bad=row;bad.fields[0].second.encoded_value="01900a09-007c-7000-8000-000000000001";
  Check(!a::ArtifactIdentityFieldsValid(bad)&&a::FieldUuid(bad,"object_uuid").is_nil());
  bad=row;bad.fields.push_back(row.fields.front());Check(!a::ArtifactIdentityFieldsValid(bad));
  bad=row;bad.fields[1].second=Uuid({});Check(a::ArtifactIdentityFieldsValid(bad));
  a::ArtifactSnapshotEntry first;first.object_uuid=Id(1);first.target_schema_uuid=Id(2);
  first.object_kind="view";first.default_name="a\nb";first.payload=std::string("c\0d",3);
  auto second=first;second.default_name="a";second.payload="b\nc";
  Check(a::SnapshotSignature(first)!=a::SnapshotSignature(second));
  second=first;second.target_schema_uuid=Id(3);
  Check(a::StableArtifactHash(first)!=a::StableArtifactHash(second));
  Check(a::SnapshotMap({first}).contains(Id(1)));
}
