#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "aggregate", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002539"), ".aggregate",
      {api::kSblrAggregateExecutorId, api::kSblrAggregateOpcodeCode,
       api::kSblrAggregateOpcodeVersion,
       api::kSblrAggregateOperandDescriptorId,
       api::kSblrAggregateResultDescriptorId,
       api::kSblrAggregateResultDescriptorVersion});
}
