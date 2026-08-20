#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_advisory_lock_release_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAdvisoryLockReleaseCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAdvisoryLockReleaseDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAdvisoryLockReleaseCoordinationResult CompileSblrAdvisoryLockReleaseDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAdvisoryLockReleaseCoordinationResult ConsumeSblrAdvisoryLockReleaseDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAdvisoryLockReleaseDescriptorV1&);}
