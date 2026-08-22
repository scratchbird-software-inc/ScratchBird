#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_dictionary_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterDictionaryCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterDictionaryDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterDictionaryCoordinationResult CompileSblrDdlAlterDictionaryDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterDictionaryCoordinationResult ConsumeSblrDdlAlterDictionaryDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterDictionaryDescriptorV1&); }
