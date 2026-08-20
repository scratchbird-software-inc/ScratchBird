#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_advisory_lock_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAdvisoryLockCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAdvisoryLockDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAdvisoryLockCoordinationResult CompileSblrAdvisoryLockDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAdvisoryLockCoordinationResult ConsumeSblrAdvisoryLockDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAdvisoryLockDescriptorV1&);}
