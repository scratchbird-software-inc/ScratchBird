#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "kv_structured_read", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002563"), ".kv_structured_read",
      {a::kSblrKvStructuredReadExecutorId,8192,"1.0",a::kSblrKvStructuredReadOperandDescriptorId,a::kSblrKvStructuredReadResultDescriptorId,1});
}
