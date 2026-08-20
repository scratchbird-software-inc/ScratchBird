#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_statement_batch_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrStatementBatchCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrStatementBatchDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrStatementBatchCoordinationResult CompileSblrStatementBatchDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrStatementBatchCoordinationResult ConsumeSblrStatementBatchDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrStatementBatchDescriptorV1&);}
