#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_subscription_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateSubscriptionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateSubscriptionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateSubscriptionCoordinationResult CompileSblrDdlCreateSubscriptionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateSubscriptionCoordinationResult ConsumeSblrDdlCreateSubscriptionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateSubscriptionDescriptorV1&);}
