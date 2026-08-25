#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_describe_capabilities_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeDescribeCapabilitiesCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeDescribeCapabilitiesDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeDescribeCapabilitiesCoordinationResult CompileSblrBridgeDescribeCapabilitiesDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeDescribeCapabilitiesCoordinationResult ConsumeSblrBridgeDescribeCapabilitiesDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeDescribeCapabilitiesDescriptorV1&); }
