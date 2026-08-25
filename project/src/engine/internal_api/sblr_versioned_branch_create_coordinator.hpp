#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_branch_create_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrVersionedBranchCreateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedBranchCreateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrVersionedBranchCreateCoordinationResult CompileSblrVersionedBranchCreateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrVersionedBranchCreateCoordinationResult ConsumeSblrVersionedBranchCreateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedBranchCreateDescriptorV1&);}
