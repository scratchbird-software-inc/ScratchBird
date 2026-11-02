#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "kv_structured_mutate", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002567"), ".kv_structured_mutate",
      {a::kSblrKvStructuredMutateExecutorId,8193,"1.0",a::kSblrKvStructuredMutateOperandDescriptorId,a::kSblrKvStructuredMutateResultDescriptorId,1});
}
