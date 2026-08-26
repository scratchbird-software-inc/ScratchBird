#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_rollback_transaction_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeRollbackTransactionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeRollbackTransactionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeRollbackTransactionCoordinationResult CompileSblrBridgeRollbackTransactionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeRollbackTransactionCoordinationResult ConsumeSblrBridgeRollbackTransactionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeRollbackTransactionDescriptorV1&); }
