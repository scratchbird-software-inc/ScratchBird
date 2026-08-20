#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_domain_operation_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDomainOperationCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDomainOperationDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDomainOperationCoordinationResult CompileSblrDomainOperationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrDomainOperationCoordinationResult ConsumeSblrDomainOperationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDomainOperationDescriptorV1&);}
