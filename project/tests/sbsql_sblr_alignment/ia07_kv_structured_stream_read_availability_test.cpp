#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "kv_structured_stream_read", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002571"), ".kv_structured_stream_read",
      {a::kSblrKvStructuredStreamReadExecutorId,8195,"1.0",a::kSblrKvStructuredStreamReadOperandDescriptorId,a::kSblrKvStructuredStreamReadResultDescriptorId,1});
}
