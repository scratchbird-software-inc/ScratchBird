#pragma once
#include "sblr_ddl_create_package_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_sequence_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlDropSequenceCoordinationResult=SblrDdlCreatePackageCoordinationResult; inline SblrDdlDropSequenceCoordinationResult CompileSblrDdlDropSequenceDescriptor(const EngineRequestContext&c,const std::string&r,std::uint64_t o,std::uint32_t p,std::uint64_t a){auto x=c;x.trace_tags.push_back("private_ddl_create_package_binder");return CompileSblrDdlCreatePackageDescriptor(x,r,o,p,a);} inline SblrDdlDropSequenceCoordinationResult ConsumeSblrDdlDropSequenceDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrDdlDropSequenceDescriptorV1&v){return ConsumeSblrDdlCreatePackageDescriptor(c,v);} }
