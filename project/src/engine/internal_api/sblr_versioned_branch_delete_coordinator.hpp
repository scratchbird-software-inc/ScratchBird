#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_branch_delete_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrVersionedBranchDeleteCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedBranchDeleteDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrVersionedBranchDeleteCoordinationResult CompileSblrVersionedBranchDeleteDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrVersionedBranchDeleteCoordinationResult ConsumeSblrVersionedBranchDeleteDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedBranchDeleteDescriptorV1&);}
