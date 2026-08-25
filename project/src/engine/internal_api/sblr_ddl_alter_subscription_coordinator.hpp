#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_subscription_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlAlterSubscriptionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterSubscriptionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlAlterSubscriptionCoordinationResult CompileSblrDdlAlterSubscriptionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlAlterSubscriptionCoordinationResult ConsumeSblrDdlAlterSubscriptionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterSubscriptionDescriptorV1&);}
