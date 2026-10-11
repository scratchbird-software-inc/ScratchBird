// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_error_vector_descriptor_registry.hpp"
#include "sblr_source_map_descriptor_registry.hpp"
#include <utility>

namespace scratchbird::engine::internal_api {
enum class SblrStartupDescriptorRegistryV1 { error_vector, source_map };
struct SblrStartupDescriptorRecoveryResultV1 {
  SblrStartupDescriptorRegistryV1 registry;
  EngineApiDiagnostic diagnostic;
  bool ok() const { return !diagnostic.error; }
};

// Private node-startup recovery after ownership and normal-mode admission.
// This does not create an ABI engine instance or certify MGA node finality.
// The operations validate the exact database binding independently. A second
// stage failure does not roll back the first stage's durable revocations.
inline SblrStartupDescriptorRecoveryResultV1 RecoverSblrStartupDescriptorsV1(
    const EngineRequestContext& context) {
  auto error_vectors = RecoverSblrErrorVectorDescriptorRegistryV1(context);
  if (error_vectors.error)
    return {SblrStartupDescriptorRegistryV1::error_vector, std::move(error_vectors)};
  return {SblrStartupDescriptorRegistryV1::source_map,
          RecoverSblrSourceMapDescriptorRegistryV1(context)};
}
}  // namespace scratchbird::engine::internal_api
