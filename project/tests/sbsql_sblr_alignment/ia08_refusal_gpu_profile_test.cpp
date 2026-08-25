#include "engine/sblr/sblr_opcode_registry.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  const auto* op = LookupSblrOperation("op.gpu.profile_disable");
  assert(op != nullptr);
  assert(op->support == SblrOpcodeSupport::implemented);
  const auto* refusal = LookupSblrOperation("diagnostic.refusal");
  assert(refusal != nullptr);
  assert(refusal->opcode == "SBLR_DIAGNOSTIC_REFUSAL");
  return 0;
}
