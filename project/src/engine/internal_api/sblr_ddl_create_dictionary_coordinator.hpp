#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_dictionary_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateDictionaryCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateDictionaryDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateDictionaryCoordinationResult CompileSblrDdlCreateDictionaryDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateDictionaryCoordinationResult ConsumeSblrDdlCreateDictionaryDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateDictionaryDescriptorV1&); }
