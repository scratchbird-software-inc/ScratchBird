#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_rewrite_rule_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropRewriteRuleCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropRewriteRuleDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropRewriteRuleCoordinationResult CompileSblrDdlDropRewriteRuleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropRewriteRuleCoordinationResult ConsumeSblrDdlDropRewriteRuleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropRewriteRuleDescriptorV1&); }
