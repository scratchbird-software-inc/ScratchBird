#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_purge_system_history_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlPurgeSystemHistoryCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlPurgeSystemHistoryCoordinationResult CompileSblrDdlPurgeSystemHistoryDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlPurgeSystemHistoryCoordinationResult ConsumeSblrDdlPurgeSystemHistoryDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryDescriptorV1&); }
