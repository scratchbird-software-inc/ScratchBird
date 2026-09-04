#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "return_result_set", "019d0000-0000-7000-8000-000000002559",
      ".return_result_set",
      {api::kSblrReturnResultSetExecutorId,
       api::kSblrReturnResultSetOpcodeCode,
       api::kSblrReturnResultSetOpcodeVersion,
       api::kSblrReturnResultSetOperandDescriptorId,
       api::kSblrReturnResultSetResultDescriptorId,
       api::kSblrReturnResultSetResultDescriptorVersion});
}
