#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_dictionary_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropDictionaryCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropDictionaryDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropDictionaryCoordinationResult CompileSblrDdlDropDictionaryDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropDictionaryCoordinationResult ConsumeSblrDdlDropDictionaryDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropDictionaryDescriptorV1&); }
