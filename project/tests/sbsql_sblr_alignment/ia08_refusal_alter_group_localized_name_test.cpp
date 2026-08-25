#include "engine/sblr/sblr_opcode_registry.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  const auto group = LookupSblrOpcode("SBLR_DDL_ALTER_GROUP");
  const auto localized = LookupSblrOpcode("SBLR_DDL_ALTER_LOCALIZED_NAME");
  assert(group && localized);
  assert(group->support == SblrOpcodeSupport::local_profile_refusal);
  assert(localized->support == SblrOpcodeSupport::local_profile_refusal);
  assert(group->transaction_effect == SblrOpcodeTransactionEffect::none);
  assert(localized->transaction_effect == SblrOpcodeTransactionEffect::none);
  assert(group->security_class == SblrOpcodeSecurityClass::authenticated);
  assert(localized->security_class == SblrOpcodeSecurityClass::authenticated);
  return 0;
}
