#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_commit_transaction_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeCommitTransactionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeCommitTransactionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeCommitTransactionCoordinationResult CompileSblrBridgeCommitTransactionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeCommitTransactionCoordinationResult ConsumeSblrBridgeCommitTransactionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeCommitTransactionDescriptorV1&); }
