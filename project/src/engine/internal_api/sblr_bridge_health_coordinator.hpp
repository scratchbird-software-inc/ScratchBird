#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_health_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeHealthCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeHealthDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeHealthCoordinationResult CompileSblrBridgeHealthDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeHealthCoordinationResult ConsumeSblrBridgeHealthDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeHealthDescriptorV1&); }
