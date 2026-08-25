#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_subscription_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropSubscriptionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropSubscriptionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropSubscriptionCoordinationResult CompileSblrDdlDropSubscriptionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropSubscriptionCoordinationResult ConsumeSblrDdlDropSubscriptionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropSubscriptionDescriptorV1&);}
