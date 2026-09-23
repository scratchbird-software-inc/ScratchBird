// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/dml_executable_trigger_runtime.hpp"
#include <cstdlib>
#include <unordered_set>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value) { if (!value) std::abort(); }
int main() {
  a::EngineRequestContext context;
  context.database_path = std::string("db|path\0suffix",14);
  context.database_uuid.bytes[0]=1;
  context.principal_uuid.bytes[0]=2;
  context.current_role_uuid.bytes[0]=3;
  a::EngineUuid table;table.bytes[0]=4;
  auto Key=[&](const a::EngineRequestContext& c,const a::EngineUuid& t) {
    return a::dml_trigger_runtime::ActiveTriggerDescriptorCacheKey(c,t);
  };
  const auto baseline=Key(context,table);
  std::unordered_set<std::string> keys{baseline};
  for(unsigned field=0;field<4;++field) for(unsigned bit=0;bit<128;++bit) {
    auto changed=context;auto target=table;
    auto* identity=field==0?&changed.database_uuid:field==1?&changed.principal_uuid:
                   field==2?&changed.current_role_uuid:&target;
    identity->bytes[bit/8]^=1u<<(bit%8);
    const auto key=Key(changed,target);
    Check(key!=baseline&&keys.insert(key).second);
  }
  for(unsigned field=0;field<3;++field) for(unsigned bit=0;bit<64;++bit) {
    auto changed=context;
    auto* epoch=field==0?&changed.catalog_generation_id:field==1?&changed.security_epoch:&changed.resource_epoch;
    *epoch^=std::uint64_t{1}<<bit;
    Check(keys.insert(Key(changed,table)).second);
  }
  auto changed=context;changed.database_path="db|path";
  Check(Key(changed,table)!=baseline);
  changed.database_path=std::string("db|path\0suffix|",15);
  Check(Key(changed,table)!=baseline);
  changed=context;changed.database_path[2]='\0';
  Check(Key(changed,table)!=baseline);
  Check(Key(context,table)==baseline);
}
