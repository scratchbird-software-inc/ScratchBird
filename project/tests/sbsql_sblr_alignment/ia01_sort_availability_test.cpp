#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "sort", "019d0000-0000-7000-8000-000000002547", ".sort",
      {api::kSblrSortExecutorId, api::kSblrSortOpcodeCode,
       api::kSblrSortOpcodeVersion, api::kSblrSortOperandDescriptorId,
       api::kSblrSortResultDescriptorId,
       api::kSblrSortResultDescriptorVersion});
}
