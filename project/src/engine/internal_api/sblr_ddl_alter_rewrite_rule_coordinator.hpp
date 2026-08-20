#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_rewrite_rule_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterRewriteRuleCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterRewriteRuleDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterRewriteRuleCoordinationResult CompileSblrDdlAlterRewriteRuleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterRewriteRuleCoordinationResult ConsumeSblrDdlAlterRewriteRuleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterRewriteRuleDescriptorV1&); }
