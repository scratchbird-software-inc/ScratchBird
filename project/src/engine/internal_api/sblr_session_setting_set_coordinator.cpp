#include "sblr_session_setting_set_coordinator.hpp"
#include "../../core/uuid/uuid.hpp"
#include "api_diagnostics.hpp"

#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace {
using Descriptor = scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1;
using Key = std::pair<EngineUuid, decltype(Descriptor::key)>;
using Owner = std::tuple<EngineUuid, EngineUuid, EngineUuid, EngineUuid,
                         EngineUuid, EngineApiU64, EngineUuid, EngineApiU64,
                         EngineApiU64, EngineApiU64>;
struct Admission {
  Descriptor descriptor;
  Owner owner;
  SessionValueOperation operation;
};
std::mutex mutex;
std::map<Key, Admission> live;
std::set<Key> consumed;

Key DescriptorKey(const Descriptor& descriptor) {
  return {EngineUuid{descriptor.receipt}, descriptor.key};
}
Owner ReceiptOwner(const EngineRequestContext& context) {
  return {context.database_uuid, context.principal_uuid, context.session_uuid,
          context.transaction_uuid, context.statement_uuid,
          context.local_transaction_id, context.authorization_context.authority_uuid,
          context.security_epoch,
          context.authorization_context.security_context_generation,
          context.authorization_context.policy_epoch};
}
bool BoundReceipt(const EngineRequestContext& context, const EngineUuid& receipt) {
  return context.security_context_present &&
         scratchbird::core::uuid::IsEngineIdentityUuid(receipt) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(context.statement_uuid) &&
         receipt == context.statement_receipt_uuid;
}
bool SameDescriptor(const Descriptor& left, const Descriptor& right) {
  return left.receipt == right.receipt && left.key == right.key &&
         left.value == right.value &&
         left.expected_generation == right.expected_generation &&
         left.availability == right.availability;
}
EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}
}  // namespace

SblrSessionSettingSetCoordinationResult CompileSblrSessionValueDescriptor(
    SessionValueOperation operation, const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  SblrSessionSettingSetCoordinationResult result;
  if (operation > SessionValueOperation::context_get ||
      !BoundReceipt(context, receipt) || !occurrence || !availability) {
    result.diagnostic = Diagnostic("SBLR.OPERAND.INVALID",
                                  "sblr.session_setting_set.coordination_invalid");
    return result;
  }
  result.descriptor.receipt = receipt.bytes;
  result.descriptor.expected_generation = occurrence;
  result.descriptor.availability = availability;
  const auto key = DescriptorKey(result.descriptor);
  const auto owner = ReceiptOwner(context);
  std::lock_guard lock(mutex);
  if (consumed.count(key)) {
    result.diagnostic = Diagnostic("MGA.TRANSACTION.STALE",
                                  "sblr.session_setting_set.replay");
    return result;
  }
  const auto [entry, inserted] = live.emplace(key, Admission{result.descriptor, owner, operation});
  if (!inserted && (entry->second.operation != operation ||
                   entry->second.owner != owner ||
                   !SameDescriptor(entry->second.descriptor, result.descriptor))) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                  "sblr.session_setting_set.receipt_owner_mismatch");
    return result;
  }
  result.ok = true;
  return result;
}

SblrSessionSettingSetCoordinationResult ConsumeSblrSessionValueDescriptor(
    SessionValueOperation operation, const EngineRequestContext& context,
    const Descriptor& descriptor) {
  SblrSessionSettingSetCoordinationResult result;
  if (!BoundReceipt(context, EngineUuid{descriptor.receipt})) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                  "sblr.session_setting_set.hidden");
    return result;
  }
  std::lock_guard lock(mutex);
  const auto key = DescriptorKey(descriptor);
  const auto entry = live.find(key);
  if (entry == live.end()) {
    result.diagnostic = Diagnostic(consumed.count(key) ? "MGA.TRANSACTION.STALE"
                                                      : "SECURITY.ACCESS_DENIED",
                                  "sblr.session_setting_set.replay");
    return result;
  }
  if (entry->second.operation != operation ||
      entry->second.owner != ReceiptOwner(context) ||
      !SameDescriptor(entry->second.descriptor, descriptor)) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                  "sblr.session_setting_set.receipt_owner_mismatch");
    return result;
  }
  if (context.query_cancellation_requested && context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                  "sblr.session_setting_set.cancelled");
    return result;
  }
  consumed.insert(key);
  live.erase(entry);
  result.descriptor = descriptor;
  result.ok = true;
  return result;
}
SblrSessionSettingSetCoordinationResult CompileSblrSessionSettingSetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::setting_set,
      context, receipt, occurrence, availability);
}
SblrSessionSettingSetCoordinationResult ConsumeSblrSessionSettingSetDescriptor(
    const EngineRequestContext& context, const Descriptor& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::setting_set,
      context, descriptor);
}
void RetireSblrSessionSettingSetReceipt(const EngineUuid& receipt) {
  std::lock_guard lock(mutex);
  const Key first{receipt, {}};
  auto pending = live.lower_bound(first);
  while (pending != live.end() && pending->first.first == receipt)
    pending = live.erase(pending);
  auto finished = consumed.lower_bound(first);
  while (finished != consumed.end() && finished->first == receipt)
    finished = consumed.erase(finished);
}
}  // namespace scratchbird::engine::internal_api
