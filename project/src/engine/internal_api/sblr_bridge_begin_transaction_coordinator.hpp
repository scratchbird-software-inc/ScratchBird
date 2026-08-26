#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_begin_transaction_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeBeginTransactionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeBeginTransactionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeBeginTransactionCoordinationResult CompileSblrBridgeBeginTransactionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeBeginTransactionCoordinationResult ConsumeSblrBridgeBeginTransactionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeBeginTransactionDescriptorV1&); }
