#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_rewrite_rule_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateRewriteRuleCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateRewriteRuleDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateRewriteRuleCoordinationResult CompileSblrDdlCreateRewriteRuleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateRewriteRuleCoordinationResult ConsumeSblrDdlCreateRewriteRuleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateRewriteRuleDescriptorV1&); }
