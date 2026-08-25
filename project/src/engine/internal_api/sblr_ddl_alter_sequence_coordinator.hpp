#pragma once
#include "sblr_ddl_create_package_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_sequence_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlAlterSequenceCoordinationResult=SblrDdlCreatePackageCoordinationResult; SblrDdlAlterSequenceCoordinationResult CompileSblrDdlAlterSequenceDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterSequenceCoordinationResult ConsumeSblrDdlAlterSequenceDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterSequenceDescriptorV1&); }
