#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "kv_structured_timeseries", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002571"), ".kv_structured_timeseries",
      {a::kSblrKvStructuredTimeseriesExecutorId,8197,"1.0",a::kSblrKvStructuredTimeseriesOperandDescriptorId,a::kSblrKvStructuredTimeseriesResultDescriptorId,1});
}
