#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "window", "019d0000-0000-7000-8000-000000002555", ".window",
      {api::kSblrWindowExecutorId, api::kSblrWindowOpcodeCode,
       api::kSblrWindowOpcodeVersion, api::kSblrWindowOperandDescriptorId,
       api::kSblrWindowResultDescriptorId,
       api::kSblrWindowResultDescriptorVersion});
}
