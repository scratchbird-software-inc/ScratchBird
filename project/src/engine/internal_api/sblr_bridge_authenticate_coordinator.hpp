#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_authenticate_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeAuthenticateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeAuthenticateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeAuthenticateCoordinationResult CompileSblrBridgeAuthenticateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeAuthenticateCoordinationResult ConsumeSblrBridgeAuthenticateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeAuthenticateDescriptorV1&); }
