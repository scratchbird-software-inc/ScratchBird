#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_database_create_template_clone_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDatabaseCreateTemplateCloneCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDatabaseCreateTemplateCloneCoordinationResult CompileSblrDatabaseCreateTemplateCloneDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDatabaseCreateTemplateCloneCoordinationResult ConsumeSblrDatabaseCreateTemplateCloneDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneDescriptorV1&); }
