#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_verifiable_history_prove_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrVerifiableHistoryProveCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVerifiableHistoryProveDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrVerifiableHistoryProveCoordinationResult CompileSblrVerifiableHistoryProveDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrVerifiableHistoryProveCoordinationResult ConsumeSblrVerifiableHistoryProveDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVerifiableHistoryProveDescriptorV1&); }
