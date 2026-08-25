#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_collation_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlAlterCollationCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterCollationDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlAlterCollationCoordinationResult CompileSblrDdlAlterCollationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlAlterCollationCoordinationResult ConsumeSblrDdlAlterCollationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterCollationDescriptorV1&);}
