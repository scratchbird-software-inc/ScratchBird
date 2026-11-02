#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace a = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireAdminBootstrapInstalls(
      "ddl_create_domain", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002571"), ".ddl_create_domain",
      {a::kSblrDdlCreateDomainExecutorId,1542,"1.0",a::kSblrDdlCreateDomainOperandDescriptorId,a::kSblrDdlCreateDomainResultDescriptorId,1});
}
