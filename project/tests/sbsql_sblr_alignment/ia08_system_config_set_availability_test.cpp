#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "system_config_set", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002571"), ".system_config_set",
      {a::kSblrSystemConfigSetExecutorId,5125,"1.0",a::kSblrSystemConfigSetOperandDescriptorId,a::kSblrSystemConfigSetResultDescriptorId,1});
}
