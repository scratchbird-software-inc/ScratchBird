#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_dml_conditional_mutate_runtime.hpp"

namespace scratchbird::engine::internal_api {

struct SblrDmlConditionalMutateCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDmlConditionalMutateDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDmlConditionalMutateCoordinationResult
CompileSblrDmlConditionalMutateDescriptor(const EngineRequestContext&,
                                          const std::string& receipt,
                                          std::uint64_t structural_occurrence,
                                          std::uint64_t mutation_occurrence,
                                          std::uint64_t availability_generation);

SblrDmlConditionalMutateCoordinationResult
ConsumeSblrDmlConditionalMutateDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDmlConditionalMutateDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
