// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_NATIVE_AGENT_RECORDS (owner Q68 = 1, Q69 = 1, Q70 = 2,
// Q71 = 1, Q72 = 1, Q73 = 1).
// Typed native agent policy / registration / per-family config value blocks:
// independent hand-built SBCV byte oracles (policy, registration and three
// family configs; no production schema or encoder), round trips of every row
// DefaultAgentBootstrapDeclarations writes today and of every family's
// default config through the boundary adapters, the generated field tables
// checked against AgentPolicySchemaForFamily, the exact set of 13 one-value
// "baseline" enums (3c, owner Q71 = 1; formerly the unrepresented set), the
// three budget fields that are policy-record only (3c, owner Q73 = 1), and
// refusals.
#include "core/catalog/catalog_native_agent_record.hpp"
#include "core/agents/agent_bootstrap_configuration.hpp"
#include "core/agents/agent_policy_schema.hpp"
#include "bootstrap_policy_fixture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>
thread_local long allocation_budget = -1;
thread_local unsigned allocation_count = 0;
void* operator new(std::size_t size) {
  ++allocation_count;
  if (allocation_budget >= 0 && allocation_budget-- == 0) throw std::bad_alloc();
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace a = scratchbird::core::agents;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace f = scratchbird::tests::bootstrap_policy;
namespace {
unsigned checks = 0;
void Check(bool ok, const std::string& why, std::source_location loc = std::source_location::current()) {
  ++checks;
  if (!ok) {
    allocation_budget = -1;
    throw std::runtime_error(why + " line=" + std::to_string(loc.line()));
  }
}
template <class Call>
auto NoHeap(Call&& call) {
  allocation_budget = 0;
  allocation_count = 0;
  auto result = call();
  const auto calls = allocation_count;
  allocation_budget = -1;
  Check(!calls, "bounded native agent codecs never allocate");
  return result;
}
p::TypedUuid Tx(unsigned n) { return {p::UuidKind::transaction, f::Id(n)}; }
std::string FamilyTag(c::CatalogAgentPolicyFamily family) {
  for (const auto& identity : c::kCatalogAgentPolicyFamilies)
    if (identity.family == family) return std::string(identity.tag);
  return {};
}
std::string SourceTypeName(c::NativeAgentConfigSourceType t) {
  using S = c::NativeAgentConfigSourceType;
  switch (t) {
    case S::boolean: return "boolean";
    case S::unsigned_integer: return "unsigned_integer";
    case S::decimal: return "decimal";
    case S::duration_seconds: return "duration_seconds";
    case S::duration_microseconds: return "duration_microseconds";
    case S::byte_count: return "byte_count";
    case S::percent: return "percent";
    case S::token: return "token";
    case S::token_list: return "token_list";
  }
  return "unknown";
}

// ---- independent oracle: hand-built SBCV with wire overrides ----
struct Wire {
  std::set<unsigned> omit;
  std::map<unsigned, p::u64> number;
  std::map<unsigned, std::string> opaque;
  std::map<unsigned, std::string> boolean;
  std::map<unsigned, p::u64> append;
} wire;
struct G {
  f::Golden g;
  explicit G(unsigned schema) : g(schema) {}
  void Id(unsigned tag, const p::Uuid& id) {
    if (!wire.omit.count(tag)) g.Identity(tag, id);
  }
  void Int(unsigned tag, p::u64 v) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.number.find(tag); it != wire.number.end()) v = it->second;
    g.Integer(tag, v);
  }
  void Bool(unsigned tag, bool v) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.boolean.find(tag); it != wire.boolean.end()) return g.Field(tag, 2, it->second);
    g.Boolean(tag, v);
  }
  void Opaque(unsigned tag, const std::string& b) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.opaque.find(tag); it != wire.opaque.end()) return g.Field(tag, 4, it->second);
    g.Field(tag, 4, b);
  }
  std::string Finish() {
    for (const auto& [tag, v] : wire.append) g.Integer(tag, v);
    return g.Finish();
  }
};
// Little-endian u16 codes, and the binary64 bit pattern little-endian.
std::string U16(std::initializer_list<unsigned> codes) {
  std::string out;
  for (const auto code : codes) {
    out.push_back(static_cast<char>(code & 0xff));
    out.push_back(static_cast<char>(code >> 8));
  }
  return out;
}
template <class L>
std::string Codes(const L& list) {
  std::string out;
  for (const auto v : list.values()) {
    const unsigned code = static_cast<p::u16>(v);
    out.push_back(static_cast<char>(code & 0xff));
    out.push_back(static_cast<char>(code >> 8));
  }
  return out;
}
std::string Binary64(double v) {
  const auto bits = std::bit_cast<p::u64>(v);
  std::string out(8, 0);
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<char>(bits >> (8 * i));
  return out;
}
std::string Golden(const c::NativeAgentPolicyRecord& r) {
  G g(65640);
  g.Id(1, r.definition_uuid);
  g.Id(2, r.database_uuid);
  g.Id(3, r.policy_uuid);
  g.Id(4, r.origin_transaction_uuid.value);
  g.Int(5, r.origin_local_transaction_id);
  g.Int(6, r.generation);
  g.Int(7, p::u64(r.agent_type));
  g.Int(8, p::u64(r.policy_family));
  g.Int(9, p::u64(r.policy_class));
  g.Int(10, r.policy_generation);
  g.Int(11, p::u64(r.layer));
  g.Int(12, p::u64(r.deployment));
  g.Int(13, p::u64(r.scope));
  g.Int(14, p::u64(r.authority));
  g.Bool(15, r.enabled);
  g.Int(16, p::u64(r.activation));
  g.Int(17, p::u64(r.action_mode));
  g.Int(18, p::u64(r.invalid_policy_behavior));
  g.Bool(19, r.allow_live_action);
  g.Bool(20, r.require_manual_approval);
  g.Bool(21, r.require_dry_run_before_live);
  g.Bool(22, r.evidence_required);
  g.Bool(23, r.explainability_required);
  g.Bool(24, r.editable_seed_policy);
  g.Int(25, r.run_interval_microseconds);
  g.Int(26, r.jitter_microseconds);
  g.Int(27, r.lease_microseconds);
  g.Int(28, r.cooldown_microseconds);
  g.Int(29, r.max_runtime_microseconds);
  g.Int(30, r.max_restart_attempts);
  g.Int(31, r.initial_backoff_microseconds);
  g.Int(32, r.max_backoff_microseconds);
  g.Int(33, r.max_history_query_rows);
  g.Int(34, r.max_evidence_fanout);
  g.Int(35, r.max_label_cardinality);
  g.Int(36, r.action_budget_per_window);
  g.Opaque(37, Codes(r.required_metric_families));
  g.Opaque(38, Codes(r.policy_dependencies));
  if (r.startup_verification) {
    g.Id(39, r.startup_verification->repair_policy_uuid);
    g.Id(40, r.startup_verification->quarantine_policy_uuid);
    g.Id(41, r.startup_verification->salvage_policy_uuid);
    g.Id(42, r.startup_verification->evidence_policy_uuid);
    g.Int(43, r.startup_verification->verification_levels);
  }
  return g.Finish();
}
std::string Golden(const c::NativeAgentRegistrationRecord& r) {
  G g(65641);
  g.Id(1, r.definition_uuid);
  g.Id(2, r.database_uuid);
  g.Id(3, r.policy_uuid);
  g.Id(4, r.origin_transaction_uuid.value);
  g.Int(5, r.origin_local_transaction_id);
  g.Int(6, r.generation);
  g.Int(7, p::u64(r.agent_type));
  g.Opaque(8, Codes(r.policy_families));
  g.Int(9, p::u64(r.state));
  g.Int(10, p::u64(r.managed_by));
  g.Int(11, p::u64(r.launch_trigger));
  g.Bool(12, r.starts_during_create);
  return g.Finish();
}
void CommonOracle(G& g, const c::NativeAgentConfigResourceBudget& b) {
  if (b.protect_foreground_work) g.Bool(64, *b.protect_foreground_work);
  const std::optional<p::u64> numbers[]{b.max_cpu_time_microseconds, b.max_memory_bytes, b.max_io_bytes,
                                        b.max_io_ops, b.max_thread_slots, b.max_queue_depth,
                                        b.min_run_interval_microseconds, b.retry_backoff_microseconds,
                                        b.watchdog_timeout_microseconds};
  // 3c (owner Q73 = 1): tags 74..76 (max_history_query_rows, max_evidence_fanout,
  // max_label_cardinality) are reserved, unused; those live on the policy record.
  for (unsigned i = 0; i < 9; ++i)
    if (numbers[i]) g.Int(65 + i, *numbers[i]);
}
template <class R>
void HeadOracle(G& g, const R& r) {
  g.Id(1, r.definition_uuid);
  g.Id(2, r.origin_transaction_uuid.value);
  g.Int(3, r.origin_local_transaction_id);
}
std::string Golden(const c::NativeAgentConfig_agent_coordinator_policy& r) {
  G g(65669);
  HeadOracle(g, r);
  g.Bool(4, r.startup_launch_enabled);
  g.Int(5, p::u64(r.startup_verification_class));
  g.Int(6, r.startup_clock_max_backward_ns);
  g.Int(7, r.startup_clock_max_forward_jump_ns);
  g.Bool(8, r.startup_clock_fail_on_rollback);
  g.Bool(9, r.startup_clock_fail_on_monotonic_regression);
  g.Bool(10, r.startup_clock_fail_on_forward_jump);
  g.Int(11, r.startup_uuid_max_regression_ms);
  g.Int(12, r.startup_uuid_same_ms_wait_timeout_ms);
  g.Bool(13, r.go_no_go_required);
  g.Opaque(14, Codes(r.managed_layers));
  g.Bool(15, r.worker_launch_forbidden);
  g.Int(16, r.max_agents_per_cycle);
  g.Int(17, r.max_schedule_queue_depth);
  g.Int(18, r.schedule_interval_microseconds);
  g.Int(19, p::u64(r.startup_agent));
  g.Int(20, p::u64(r.shutdown_agent));
  g.Bool(21, r.coordinator_stops_last);
  CommonOracle(g, r.common);
  return g.Finish();
}
std::string Golden(const c::NativeAgentConfig_agent_startup_policy& r) {
  G g(65670);
  HeadOracle(g, r);
  g.Int(4, p::u64(r.launch_trigger));
  g.Bool(5, r.engine_verification_required);
  g.Bool(6, r.committed_policy_required);
  g.Bool(7, r.child_readiness_required);
  g.Bool(8, r.configured_components_only);
  g.Opaque(9, Codes(r.dependency_order));
  g.Int(10, r.readiness_timeout_microseconds);
  g.Int(11, r.max_retry_attempts);
  CommonOracle(g, r.common);
  return g.Finish();
}
// 3c (owner Q71 = 1): a Token and a TokenList "baseline" field, each a
// one-value u16 enum stored as one unsigned integer in its reserved tag.
std::string Golden(const c::NativeAgentConfig_backup_policy& r) {
  G g(65660);
  HeadOracle(g, r);
  g.Int(4, r.stuck_progress_window);
  g.Bool(5, r.verification_required);
  g.Int(6, p::u64(r.blocker_policy));
  g.Bool(7, r.cancel_requires_approval);
  CommonOracle(g, r.common);
  return g.Finish();
}
std::string Golden(const c::NativeAgentConfig_admission_control_policy& r) {
  G g(65648);
  HeadOracle(g, r);
  g.Int(4, r.listener_queue_threshold);
  g.Int(5, r.scheduler_queue_threshold);
  g.Int(6, r.hard_limit_threshold);
  g.Int(7, p::u64(r.workload_class_rules));
  CommonOracle(g, r.common);
  return g.Finish();
}
std::string Golden(const c::NativeAgentConfig_policy_recommendation_policy& r) {
  G g(65653);
  HeadOracle(g, r);
  g.Opaque(4, Binary64(r.recommendation_burn_rate));
  g.Opaque(5, Binary64(r.confidence_threshold));
  g.Int(6, r.cooldown_seconds);
  g.Bool(7, r.no_auto_apply);
  CommonOracle(g, r.common);
  return g.Finish();
}
template <class R>
std::string GoldenWith(const R& r, std::function<void(Wire&)> set) {
  wire = {};
  set(wire);
  auto out = Golden(r);
  wire = {};
  return out;
}

// ---- codec table ----
template <class R>
struct Codec {
  std::function<std::optional<std::size_t>(const R&)> bytes;
  std::function<c::CatalogValueEncodeIntoResult(const R&, std::span<p::byte>)> into;
  std::function<c::CatalogValueEncodeResult(const R&)> owning;
  std::function<c::NativeAgentDecodeResult<R>(std::string_view)> decode;
  std::size_t max;
};
template <class R>
Codec<R> CodecOf() {
  if constexpr (std::is_same_v<R, c::NativeAgentPolicyRecord>)
    return {c::NativeAgentPolicyEncodedBytes, c::EncodeNativeAgentPolicyInto, c::EncodeNativeAgentPolicy,
            c::DecodeNativeAgentPolicy, c::kNativeAgentPolicyMaxBytes};
  else if constexpr (std::is_same_v<R, c::NativeAgentRegistrationRecord>)
    return {c::NativeAgentRegistrationEncodedBytes, c::EncodeNativeAgentRegistrationInto,
            c::EncodeNativeAgentRegistration, c::DecodeNativeAgentRegistration, c::kNativeAgentRegistrationMaxBytes};
  else
    return {c::NativeAgentConfigEncodedBytes<R>, c::EncodeNativeAgentConfigInto<R>, c::EncodeNativeAgentConfig<R>,
            c::DecodeNativeAgentConfig<R>, R::max_bytes};
}
template <class R>
concept HasOracle = requires(const R& r) { Golden(r); };

std::string_view View(std::span<const p::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
// Round trip: exact oracle bytes when an oracle exists, else the codec's own
// bytes; complete field retention through decode; truncation and trailing
// bytes refused; short output never writes.
template <class R>
std::string Roundtrip(const R& value) {
  const auto k = CodecOf<R>();
  std::array<p::byte, 2048> output;
  output.fill(0xa7);
  const auto size = NoHeap([&] { return k.bytes(value); });
  Check(size && *size <= k.max, "complete byte count within the registered maximum");
  const auto encoded = NoHeap([&] { return k.into(value, std::span(output).subspan(1, *size + 3)); });
  Check(encoded.ok() && encoded.bytes.size() == *size, "encode into an unaligned destination");
  const std::string bytes(View(encoded.bytes));
  if constexpr (HasOracle<R>) Check(bytes == Golden(value), "exact independent SBCV bytes");
  Check(output.front() == 0xa7 && std::all_of(output.begin() + bytes.size() + 1, output.end(),
                                              [](auto b) { return b == 0xa7; }),
        "unaligned destination preserves guards");
  const auto owned = k.owning(value);
  Check(owned.ok() && View(owned.bytes) == bytes, "owning encode equals encode-into");
  const auto decoded = NoHeap([&] { return k.decode(bytes); });
  Check(decoded.ok() && *decoded.record == value, "every field retained through decode");
  for (std::size_t n = 0; n < bytes.size(); n += (bytes.size() > 400 ? 7 : 1))
    Check(!NoHeap([&] { return k.decode(std::string_view(bytes).substr(0, n)); }).ok(), "truncated wire refused");
  Check(!k.decode(bytes + 'x').ok(), "trailing byte refused");
  std::array<p::byte, 2048> shorter;
  shorter.fill(0xab);
  const auto before = shorter;
  const auto short_write = NoHeap([&] { return k.into(value, std::span(shorter).first(bytes.size() - 1)); });
  Check(!short_write.ok() && short_write.bytes.empty() && shorter == before, "short output never writes a prefix");
  return bytes;
}
template <class R>
void Refuse(const R& value, bool wire_too = true) {
  const auto k = CodecOf<R>();
  std::array<p::byte, 2048> output;
  output.fill(0xa7);
  const auto before = output;
  const auto encoded = NoHeap([&] { return k.into(value, output); });
  Check(!encoded.ok() && encoded.bytes.empty() && output == before, "invalid record leaves output unchanged");
  Check(!NoHeap([&] { return k.bytes(value); }), "invalid record has no encoded size");
  Check(!k.owning(value).ok(), "owning encode refuses invalid record");
  if constexpr (HasOracle<R>) {
    if (wire_too) {
      const auto raw = Golden(value);
      Check(!NoHeap([&] { return k.decode(raw); }).ok(), "invalid record bytes refused on decode");
    }
  }
}
template <class R>
void RefuseWire(const std::string& raw, const char* why) {
  const auto decoded = NoHeap([&] { return CodecOf<R>().decode(raw); });
  Check(!decoded.ok() && !decoded.record, why);
}
template <class R>
void RequiredOnWire(const R& full, std::initializer_list<unsigned> required) {
  for (unsigned tag : required)
    RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.omit.insert(tag); }),
                  "missing required field refused");
  RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.append[99] = 1; }), "unknown field refused");
}
template <class R>
void Framing(const R& base) {
  const auto raw = Golden(base);
  for (const unsigned offset : {0u, 4u, 6u, 8u, 12u, 16u, 20u, 22u, 24u, 26u}) {
    auto invalid = raw;
    invalid[offset] ^= 0x7f;
    RefuseWire<R>(invalid, "invalid framing/schema/tag/type refused");
  }
}

// ---- property text -> tags ----
std::vector<c::NativeAgentPropertyTag> Tags(std::string_view properties, bool config, std::vector<std::string>& keep) {
  std::vector<c::NativeAgentPropertyTag> out;
  while (!properties.empty()) {
    const auto end = properties.find('\n');
    const auto line = properties.substr(0, end);
    const auto equal = line.find('=');
    const auto key = line.substr(0, equal), value = line.substr(equal + 1);
    if (key.starts_with("config.") == config)
      out.push_back({config ? key.substr(7) : key, value});
    properties.remove_prefix(end + 1);
  }
  (void)keep;
  return out;
}
std::vector<c::NativeAgentPropertyTag> MapTags(const std::map<std::string, std::string>& m) {
  std::vector<c::NativeAgentPropertyTag> out;
  for (const auto& [k, v] : m) out.push_back({k, v});
  return out;
}

// ---- policy ----
c::NativeAgentPolicyRecord Policy() {
  c::NativeAgentPolicyRecord r;
  r.definition_uuid = r.policy_uuid = f::Id(301);
  r.database_uuid = f::Id(7);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.generation = 1;
  r.agent_type = c::CatalogAgentType::memory_governor;
  r.policy_family = c::CatalogAgentPolicyFamily::memory_governor_policy;
  r.policy_class = c::CatalogAgentPolicyClass::operational_agent;
  r.policy_generation = 1;
  r.layer = c::CatalogAgentRuntimeLayer::l3_dispatcher;
  r.deployment = c::CatalogAgentDeployment::local;
  r.scope = c::CatalogAgentScope::node | c::CatalogAgentScope::database;
  r.authority = c::CatalogAgentAuthorityClass::request_action;
  r.enabled = true;
  r.activation = c::CatalogAgentActivationProfile::dry_run;
  r.action_mode = c::CatalogAgentActionMode::request_action;
  r.invalid_policy_behavior = c::CatalogAgentInvalidPolicyBehavior::conservative_grants;
  r.require_manual_approval = true;
  r.editable_seed_policy = true;
  r.run_interval_microseconds = 1000000;
  r.lease_microseconds = 5;
  r.max_restart_attempts = 3;
  r.action_budget_per_window = ~p::u64{0};
  r.required_metric_families.push(c::CatalogMetricFamily::sb_index_candidates_total);
  r.required_metric_families.push(c::CatalogMetricFamily(426));
  r.policy_dependencies.push(c::CatalogAgentPolicyFamily::node_resource_policy);
  return r;
}
c::CatalogAgentStartupVerificationSelection Startup() {
  return {f::Id(401), f::Id(402), f::Id(403), f::Id(404), 1023};
}
void Policies() {
  using P = c::NativeAgentPolicyRecord;
  const auto base = Policy();
  const auto raw = Roundtrip(base);
  Check(raw.size() == 24 + 4 * 24 + 25 * 16 + 7 * 9 + (8 + 4) + (8 + 2), "policy byte count");
  auto b = base;
  b.required_metric_families = {};
  b.policy_dependencies = {};
  Roundtrip(b);  // both lists may be empty
  // Full: every metric and family listed (reverse order kept), startup selection.
  auto full = base;
  full.agent_type = c::CatalogAgentType::agent_coordinator;
  full.policy_family = c::CatalogAgentPolicyFamily::agent_coordinator_policy;
  full.required_metric_families = {};
  for (unsigned m = 426; m >= 1; --m) full.required_metric_families.push(c::CatalogMetricFamily(m));
  full.policy_dependencies = {};
  for (unsigned m = 1; m <= 36; ++m) full.policy_dependencies.push(c::CatalogAgentPolicyFamily(m));
  full.startup_verification = Startup();
  Check(Roundtrip(full).size() == c::kNativeAgentPolicyMaxBytes, "every field at its largest = registered maximum");
  // Every value of every enumeration, every scope flag, both flag states.
  for (unsigned v = 1; v <= 32; ++v) { b = base; b.agent_type = c::CatalogAgentType(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 36; ++v) { b = base; b.policy_family = c::CatalogAgentPolicyFamily(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 2; ++v) { b = base; b.policy_class = c::CatalogAgentPolicyClass(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 5; ++v) { b = base; b.layer = c::CatalogAgentRuntimeLayer(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 3; ++v) { b = base; b.deployment = c::CatalogAgentDeployment(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 4; ++v) { b = base; b.authority = c::CatalogAgentAuthorityClass(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 5; ++v) { b = base; b.activation = c::CatalogAgentActivationProfile(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 8; ++v) { b = base; b.action_mode = c::CatalogAgentActionMode(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 27; ++v) {
    b = base; b.invalid_policy_behavior = c::CatalogAgentInvalidPolicyBehavior(v); Roundtrip(b);
  }
  for (unsigned bit = 0; bit < 25; ++bit) { b = base; b.scope = c::CatalogAgentScope(p::u64{1} << bit); Roundtrip(b); }
  b = base; b.scope = c::CatalogAgentScope((p::u64{1} << 25) - 1); Roundtrip(b);
  b = base;
  b.enabled = !b.enabled; b.allow_live_action = b.require_dry_run_before_live = b.evidence_required =
      b.explainability_required = true;
  b.require_manual_approval = b.editable_seed_policy = false;
  b.policy_generation = 0;  // the codec does not tie policy_generation to generation
  Roundtrip(b);
  // Refusals: identities, origin, generation.
  b = base; b.definition_uuid = b.policy_uuid = {}; Refuse(b);
  b = base; b.database_uuid = {}; Refuse(b);
  b = base; b.policy_uuid = f::Id(302); Refuse(b);
  b = base; b.definition_uuid.bytes[6] = 0x40; b.policy_uuid = b.definition_uuid; Refuse(b);
  b = base; b.origin_transaction_uuid.value = {}; Refuse(b);
  b = base; b.origin_transaction_uuid.kind = p::UuidKind::object; Refuse(b, false);
  b = base; b.origin_local_transaction_id = 0; Refuse(b);
  b = base; b.generation = 0; Refuse(b);
  // Enumerations zero and past their end.
  b = base; b.agent_type = c::CatalogAgentType::none; Refuse(b);
  b = base; b.agent_type = c::CatalogAgentType(33); Refuse(b);
  b = base; b.policy_family = c::CatalogAgentPolicyFamily::none; Refuse(b);
  b = base; b.policy_family = c::CatalogAgentPolicyFamily(37); Refuse(b);
  b = base; b.policy_class = c::CatalogAgentPolicyClass::none; Refuse(b);
  b = base; b.policy_class = c::CatalogAgentPolicyClass(3); Refuse(b);
  b = base; b.layer = c::CatalogAgentRuntimeLayer::none; Refuse(b);
  b = base; b.layer = c::CatalogAgentRuntimeLayer(6); Refuse(b);
  b = base; b.deployment = c::CatalogAgentDeployment::none; Refuse(b);
  b = base; b.deployment = c::CatalogAgentDeployment(4); Refuse(b);
  b = base; b.authority = c::CatalogAgentAuthorityClass::none; Refuse(b);
  b = base; b.authority = c::CatalogAgentAuthorityClass(5); Refuse(b);
  b = base; b.activation = c::CatalogAgentActivationProfile::none; Refuse(b);
  b = base; b.activation = c::CatalogAgentActivationProfile(6); Refuse(b);
  b = base; b.action_mode = c::CatalogAgentActionMode::none; Refuse(b);
  b = base; b.action_mode = c::CatalogAgentActionMode(9); Refuse(b);
  b = base; b.invalid_policy_behavior = c::CatalogAgentInvalidPolicyBehavior::none; Refuse(b);
  b = base; b.invalid_policy_behavior = c::CatalogAgentInvalidPolicyBehavior(28); Refuse(b);
  b = base; b.scope = c::CatalogAgentScope::none; Refuse(b);
  b = base; b.scope = c::CatalogAgentScope(p::u64{1} << 25); Refuse(b);
  b = base; b.scope = c::CatalogAgentScope::node | c::CatalogAgentScope(p::u64{1} << 63); Refuse(b);
  // u16 enumerations carried as u64: a wider wire value is refused, never truncated.
  for (const unsigned tag : {7u, 8u, 9u, 11u, 12u, 14u, 16u, 17u, 18u})
    RefuseWire<P>(GoldenWith(base, [&](Wire& w) { w.number[tag] = 0x10000 + p::u64(1); }),
                  "enumeration wider than u16 refused");
  // Code lists: unknown, zero, duplicate, odd length, oversize.
  b = base; b.required_metric_families.push(c::CatalogMetricFamily(427)); Refuse(b);
  b = base; b.required_metric_families.push(c::CatalogMetricFamily::none); Refuse(b);
  b = base; b.required_metric_families.push(c::CatalogMetricFamily::sb_index_candidates_total); Refuse(b);
  b = base; b.policy_dependencies.push(c::CatalogAgentPolicyFamily(37)); Refuse(b);
  b = base; b.policy_dependencies.push(c::CatalogAgentPolicyFamily::node_resource_policy); Refuse(b);
  b = base; b.policy_dependencies.count = b.policy_dependencies.capacity + 1; Refuse(b, false);
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[37] = U16({1}) + "\x01"; }), "odd-length code list refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[38] = "\x01"; }), "one-byte code list refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[37] = U16({427}); }), "unknown metric code refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[37] = U16({5, 5}); }), "duplicate metric refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[38] = U16({0}); }), "zero family code refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[38] = std::string(74, '\x01'); }),
                "family list longer than its universe refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.opaque[37] = std::string(2 * 427, '\x01'); }),
                "metric list longer than its universe refused");
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.boolean[15] = std::string(1, '\x02'); }),
                "boolean other than 0/1 refused");
  // Startup verification selection: coordinator only, same rules as the text codec.
  auto coord = base;
  coord.agent_type = c::CatalogAgentType::agent_coordinator;
  coord.startup_verification = Startup();
  Roundtrip(coord);
  for (const p::u64 levels : {p::u64{1}, p::u64{512}}) { b = coord; b.startup_verification->verification_levels = levels; Roundtrip(b); }
  b = coord; b.agent_type = c::CatalogAgentType::startup_agent; Refuse(b);
  b = coord; b.startup_verification->verification_levels = 0; Refuse(b);
  b = coord; b.startup_verification->verification_levels = 1024; Refuse(b);
  b = coord; b.startup_verification->salvage_policy_uuid = b.startup_verification->repair_policy_uuid; Refuse(b);
  b = coord; b.startup_verification->evidence_policy_uuid = b.definition_uuid; Refuse(b);
  b = coord; b.startup_verification->repair_policy_uuid = b.database_uuid; Refuse(b);
  b = coord; b.startup_verification->quarantine_policy_uuid = b.origin_transaction_uuid.value; Refuse(b);
  b = coord; b.startup_verification->quarantine_policy_uuid = {}; Refuse(b);
  for (unsigned tag = 39; tag <= 43; ++tag)
    RefuseWire<P>(GoldenWith(coord, [&](Wire& w) { w.omit.insert(tag); }), "partial startup selection refused");
  std::vector<unsigned> required;
  for (unsigned tag = 1; tag <= 38; ++tag) required.push_back(tag);
  for (unsigned tag : required) RefuseWire<P>(GoldenWith(coord, [&](Wire& w) { w.omit.insert(tag); }), "missing required field refused");
  RefuseWire<P>(GoldenWith(coord, [](Wire& w) { w.append[99] = 1; }), "unknown field refused");
  Framing(base);
  RefuseWire<c::NativeAgentRegistrationRecord>(raw, "policy schema id refused by the registration decoder");
  RefuseWire<c::NativeAgentConfig_agent_coordinator_policy>(raw, "policy schema id refused by a config decoder");
  auto overlap = base;
  Check(NoHeap([&] { return c::EncodeNativeAgentPolicyInto(overlap, {reinterpret_cast<p::byte*>(&overlap), sizeof(overlap)}); })
                .error == c::CatalogValueError::invalid_backing,
        "encoding cannot overwrite its input object");
  // Header binding and selection.
  c::CatalogTypedRecordView view{};
  view.header.kind = c::CatalogRecordKind::policy;
  view.header.object_uuid = {p::UuidKind::object, base.definition_uuid};
  view.payload = raw;
  Check(c::NativeAgentPolicyMatchesHeader(view) && c::IsNativeAgentPayload(raw), "policy header binding");
  view.header.kind = c::CatalogRecordKind::config_profile;
  Check(!c::NativeAgentPolicyMatchesHeader(view), "policy under another header kind refused");
  view.header.kind = c::CatalogRecordKind::policy;
  view.header.object_uuid.value = f::Id(999);
  Check(!c::NativeAgentPolicyMatchesHeader(view), "policy under another object refused");
}

// ---- registration ----
c::NativeAgentRegistrationRecord Registration() {
  c::NativeAgentRegistrationRecord r;
  r.definition_uuid = f::Id(501);
  r.database_uuid = f::Id(7);
  r.policy_uuid = f::Id(301);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.generation = 1;
  r.agent_type = c::CatalogAgentType::memory_governor;
  r.policy_families.push(c::CatalogAgentPolicyFamily::memory_governor_policy);
  r.state = c::CatalogAgentRegistrationState::registered;
  r.managed_by = c::CatalogAgentManagedBy::agent_coordinator;
  r.launch_trigger = c::CatalogAgentLaunchTrigger::policy_schedule;
  return r;
}
void Registrations() {
  using R = c::NativeAgentRegistrationRecord;
  const auto base = Registration();
  const auto raw = Roundtrip(base);
  Check(raw.size() == 24 + 4 * 24 + 6 * 16 + (8 + 2) + 9, "registration byte count");
  auto full = base;
  full.policy_families = {};
  for (unsigned v = 36; v >= 1; --v) full.policy_families.push(c::CatalogAgentPolicyFamily(v));
  full.starts_during_create = true;
  Check(Roundtrip(full).size() == c::kNativeAgentRegistrationMaxBytes, "registration maximum byte count");
  auto b = base;
  for (unsigned v = 1; v <= 2; ++v) { b = base; b.state = c::CatalogAgentRegistrationState(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 2; ++v) { b = base; b.managed_by = c::CatalogAgentManagedBy(v); Roundtrip(b); }
  for (unsigned v = 1; v <= 4; ++v) { b = base; b.launch_trigger = c::CatalogAgentLaunchTrigger(v); Roundtrip(b); }
  b = base; b.policy_families = {}; Refuse(b);
  b = base; b.policy_families.push(c::CatalogAgentPolicyFamily::memory_governor_policy); Refuse(b);
  b = base; b.policy_families.push(c::CatalogAgentPolicyFamily(37)); Refuse(b);
  b = base; b.agent_type = c::CatalogAgentType::none; Refuse(b);
  b = base; b.agent_type = c::CatalogAgentType(33); Refuse(b);
  b = base; b.state = c::CatalogAgentRegistrationState::none; Refuse(b);
  b = base; b.state = c::CatalogAgentRegistrationState(3); Refuse(b);
  b = base; b.managed_by = c::CatalogAgentManagedBy::none; Refuse(b);
  b = base; b.managed_by = c::CatalogAgentManagedBy(3); Refuse(b);
  b = base; b.launch_trigger = c::CatalogAgentLaunchTrigger::none; Refuse(b);
  b = base; b.launch_trigger = c::CatalogAgentLaunchTrigger(5); Refuse(b);
  b = base; b.policy_uuid = {}; Refuse(b);
  b = base; b.database_uuid = {}; Refuse(b);
  b = base; b.origin_local_transaction_id = 0; Refuse(b);
  b = base; b.generation = 0; Refuse(b);
  for (const unsigned tag : {7u, 9u, 10u, 11u})
    RefuseWire<R>(GoldenWith(base, [&](Wire& w) { w.number[tag] = 0x10002; }), "enumeration wider than u16 refused");
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[8] = ""; }), "empty family list refused on the wire");
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[8] = "\x04"; }), "odd-length family list refused");
  RequiredOnWire(base, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  Framing(base);
  RefuseWire<c::NativeAgentPolicyRecord>(raw, "registration schema id refused by the policy decoder");
  c::CatalogTypedRecordView view{};
  view.header.kind = c::CatalogRecordKind::config_profile;
  view.header.object_uuid = {p::UuidKind::object, base.definition_uuid};
  view.payload = raw;
  Check(c::NativeAgentRegistrationMatchesHeader(view) && !c::NativeAgentPolicyMatchesHeader(view),
        "registration header binding");
}

// ---- config records: oracles and refusals for three families ----
template <class R>
void SetHead(R& r, unsigned n) {
  r.definition_uuid = f::Id(n);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
}
template <class R>
R FromDefaults() {
  const auto tag = FamilyTag(R::family);
  Check(!tag.empty(), "family has a tag");
  const auto defaults = a::DefaultAgentPolicyConfigFieldsForFamily(tag);
  const auto tags = MapTags(defaults);
  const auto adapted = c::NativeAgentConfigFromTags<R>(tags);
  Check(adapted.ok(), "default config representable: " + tag + " key=" +
                          std::string(adapted.bad_key));
  auto r = *adapted.record;
  SetHead(r, 600 + static_cast<unsigned>(R::family));
  return r;
}
template <class R>
void FullCommon(R& r) {
  auto& b = r.common;
  b.protect_foreground_work = false;
  b.max_cpu_time_microseconds = b.max_memory_bytes = b.max_io_bytes = 1;
  b.max_io_ops = b.max_thread_slots = b.max_queue_depth = 0;
  b.min_run_interval_microseconds = b.retry_backoff_microseconds = b.watchdog_timeout_microseconds = ~p::u64{0};
  // 3c (owner Q73 = 1): the three history/evidence/label budgets are no longer
  // config fields (policy record tags 33-35 only).
}
template <class R>
void ConfigHeadRefusals(const R& base) {
  auto b = base; b.definition_uuid = {}; Refuse(b);
  b = base; b.origin_transaction_uuid.value = b.definition_uuid; Refuse(b);
  b = base; b.origin_transaction_uuid.kind = p::UuidKind::object; Refuse(b, false);
  b = base; b.origin_local_transaction_id = 0; Refuse(b);
  b = base; b.common.max_memory_bytes = 0; Refuse(b);              // Bytes minimum 1
  b = base; b.common.watchdog_timeout_microseconds = 0; Refuse(b);  // DurationMicros minimum 1
  b = base; b.common.max_io_ops = 0; Roundtrip(b);                 // Count minimum 0
  Framing(base);
}
void Coordinator() {
  using R = c::NativeAgentConfig_agent_coordinator_policy;
  using V = c::NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class;
  const auto base = FromDefaults<R>();
  Check(base.startup_verification_class == V::startup_full_verify && base.startup_launch_enabled &&
            base.startup_clock_max_forward_jump_ns == 60000000000ull && !base.startup_clock_fail_on_forward_jump &&
            base.startup_uuid_same_ms_wait_timeout_ms == 1000 && base.max_schedule_queue_depth == 1024 &&
            base.startup_agent == c::CatalogAgentType::startup_agent &&
            base.shutdown_agent == c::CatalogAgentType::shutdown_agent && base.managed_layers.count == 3 &&
            base.managed_layers.codes[2] == c::CatalogAgentRuntimeLayer::l3_dispatcher,
        "coordinator defaults mapped to their constants");
  const auto raw = Roundtrip(base);
  Check(raw.size() == 24 + 2 * 24 + 16 + 7 * 9 + 10 * 16 + (8 + 6), "coordinator byte count");
  auto full = base;
  FullCommon(full);
  full.managed_layers = {};
  for (unsigned v = 5; v >= 1; --v) full.managed_layers.push(c::CatalogAgentRuntimeLayer(v));
  Check(Roundtrip(full).size() == R::max_bytes, "coordinator maximum byte count");
  auto b = base;
  b.startup_verification_class = V::startup_light_verify; Roundtrip(b);
  b = base; b.startup_verification_class = V::none; Refuse(b);
  b = base; b.startup_verification_class = V(3); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.number[5] = 0x10002; }), "generated enum wider than u16 refused");
  b = base; b.startup_agent = c::CatalogAgentType::none; Refuse(b);
  b = base; b.shutdown_agent = c::CatalogAgentType(33); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.number[19] = 0x1001f; }), "agent type wider than u16 refused");
  b = base; b.managed_layers = {}; Refuse(b);  // required: nonempty
  b = base; b.managed_layers.push(c::CatalogAgentRuntimeLayer::l1_observation); Refuse(b);
  b = base; b.managed_layers.push(c::CatalogAgentRuntimeLayer(6)); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[14] = U16({1, 2, 3, 4, 5, 1}); }),
                "layer list longer than its universe refused");
  // Ranges from the schema map.
  b = base; b.startup_uuid_max_regression_ms = 0x0000ffffffffffffull; Roundtrip(b);
  b = base; b.startup_uuid_max_regression_ms = 0x0001000000000000ull; Refuse(b);
  b = base; b.startup_uuid_same_ms_wait_timeout_ms = 18446744073709ull; Roundtrip(b);
  b = base; b.startup_uuid_same_ms_wait_timeout_ms = 18446744073710ull; Refuse(b);
  b = base; b.startup_uuid_same_ms_wait_timeout_ms = 0; Refuse(b);
  b = base; b.schedule_interval_microseconds = 0; Refuse(b);
  b = base; b.startup_clock_max_backward_ns = ~p::u64{0}; b.max_agents_per_cycle = 0; Roundtrip(b);
  ConfigHeadRefusals(base);
  std::vector<unsigned> required{1, 2, 3};
  for (unsigned tag = 4; tag <= 21; ++tag) required.push_back(tag);
  for (unsigned tag : required) RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.omit.insert(tag); }), "missing required config field refused");
  RefuseWire<R>(GoldenWith(full, [](Wire& w) { w.append[99] = 1; }), "unknown config field refused");
  const auto bare = c::DecodeNativeAgentConfig<R>(GoldenWith(full, [](Wire& w) {
    for (unsigned tag = 64; tag <= 73; ++tag) w.omit.insert(tag);  // 3c: common tags end at 73 (Q73 = 1)
  }));
  Check(bare.ok() && bare.record->common == c::NativeAgentConfigResourceBudget{}, "common fields optional");
  // 3c (owner Q73 = 1): the reserved tags 74..76 are refused on the wire.
  for (unsigned tag = 74; tag <= 76; ++tag)
    RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.append[tag] = 1; }), "reserved Q73 config tag refused");
  RefuseWire<c::NativeAgentConfig_agent_startup_policy>(raw, "coordinator schema id refused by another family");
}
void StartupConfig() {
  using R = c::NativeAgentConfig_agent_startup_policy;
  const auto base = FromDefaults<R>();
  Check(base.launch_trigger == c::CatalogAgentLaunchTrigger::coordinator_startup &&
            base.dependency_order.count == 3 &&
            base.dependency_order.codes[0] == c::CatalogAgentType::memory_governor &&
            base.dependency_order.codes[1] == c::CatalogAgentType::filespace_capacity_manager &&
            base.dependency_order.codes[2] == c::CatalogAgentType::page_allocation_manager &&
            base.readiness_timeout_microseconds == 30000000 && base.max_retry_attempts == 3,
        "startup defaults mapped in order");
  Roundtrip(base);
  auto full = base;
  FullCommon(full);
  full.dependency_order = {};
  for (unsigned v = 32; v >= 1; --v) full.dependency_order.push(c::CatalogAgentType(v));
  Check(Roundtrip(full).size() == R::max_bytes, "startup maximum byte count");
  auto b = base;
  b.launch_trigger = c::CatalogAgentLaunchTrigger::none; Refuse(b);
  b = base; b.launch_trigger = c::CatalogAgentLaunchTrigger(5); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.number[4] = 0x10002; }), "launch trigger wider than u16 refused");
  b = base; b.dependency_order = {}; Refuse(b);
  b = base; b.dependency_order.push(c::CatalogAgentType::memory_governor); Refuse(b);
  b = base; b.dependency_order.push(c::CatalogAgentType(33)); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[9] = U16({6, 4}) + "\x05"; }), "odd agent type list refused");
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[9] = U16({6, 0}); }), "zero agent type refused");
  b = base; b.readiness_timeout_microseconds = 0; Refuse(b);
  ConfigHeadRefusals(base);
  RequiredOnWire(full, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
}
void RecommendationConfig() {
  using R = c::NativeAgentConfig_policy_recommendation_policy;
  const auto base = FromDefaults<R>();
  Check(base.recommendation_burn_rate == 1.0 && base.confidence_threshold == 1.0 && base.cooldown_seconds == 1 &&
            base.no_auto_apply,
        "recommendation defaults");
  Roundtrip(base);
  auto b = base;
  b.recommendation_burn_rate = 0.75; b.confidence_threshold = 0.0; Roundtrip(b);  // a value u64 could not hold
  b = base; b.confidence_threshold = std::nextafter(0.0, 1.0); Roundtrip(b);
  b = base; b.recommendation_burn_rate = std::nextafter(1.0, 2.0); Refuse(b);     // maximum 1
  b = base; b.recommendation_burn_rate = -0.25; Refuse(b);
  b = base; b.recommendation_burn_rate = -0.0; Refuse(b);
  b = base; b.confidence_threshold = std::numeric_limits<double>::quiet_NaN(); Refuse(b);
  b = base; b.confidence_threshold = std::numeric_limits<double>::infinity(); Refuse(b);
  b = base; b.cooldown_seconds = 0; Refuse(b);  // DurationSeconds minimum 1
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[4] = Binary64(0.5).substr(0, 7); }), "7-byte decimal refused");
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[4] = Binary64(0.5) + std::string(1, '\0'); }), "9-byte decimal refused");
  RefuseWire<R>(GoldenWith(base, [](Wire& w) { w.opaque[5] = Binary64(std::numeric_limits<double>::quiet_NaN()); }),
                "NaN decimal refused on the wire");
  ConfigHeadRefusals(base);
  RequiredOnWire(base, {1, 2, 3, 4, 5, 6, 7});
  // An unbounded decimal (remote_route_latency_weight: no maximum).
  auto route = FromDefaults<c::NativeAgentConfig_remote_routing_policy>();
  route.remote_route_latency_weight = 1e9;
  Roundtrip(route);
  route.remote_route_latency_weight = std::numeric_limits<double>::infinity();
  Refuse(route);
}

// ---- 3c (owner Q71 = 1): the 13 free-text "baseline" fields ----
template <class R, class En>
void BaselineField(En R::*member, unsigned tag, const char* key) {
  const auto base = FromDefaults<R>();
  Check(base.*member == En::baseline && p::u64(En::baseline) == 1, std::string("baseline default mapped: ") + key);
  Roundtrip(base);  // exact oracle bytes
  auto b = base; b.*member = En::none; Refuse(b);
  b = base; b.*member = En(2); Refuse(b);
  RefuseWire<R>(GoldenWith(base, [&](Wire& w) { w.number[tag] = 2; }), "unlisted baseline-enum value refused");
  RefuseWire<R>(GoldenWith(base, [&](Wire& w) { w.number[tag] = 0x10001; }), "baseline enum wider than u16 refused");
  RefuseWire<R>(GoldenWith(base, [&](Wire& w) { w.omit.insert(tag); }), "baseline field required on the wire");
}
void BaselineOnly() {
  using Backup = c::NativeAgentConfig_backup_policy;
  using Admission = c::NativeAgentConfig_admission_control_policy;
  BaselineField(&Backup::blocker_policy, 6, "blocker_policy");                    // Token
  BaselineField(&Admission::workload_class_rules, 7, "workload_class_rules");      // TokenList
}
// ---- 3c (owner Q73 = 1): three budgets are policy-record fields only ----
void PolicyRecordBudgets() {
  using R = c::NativeAgentConfig_agent_coordinator_policy;
  const auto defaults = a::DefaultAgentPolicyConfigFieldsForFamily("agent_coordinator_policy");
  const auto plain = c::NativeAgentConfigFromTags<R>(MapTags(defaults));
  Check(plain.ok(), "coordinator defaults");
  auto policy = Policy();
  policy.max_history_query_rows = 1024; policy.max_evidence_fanout = 128; policy.max_label_cardinality = 256;
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"max_history_query_rows", "1024"}, {"max_evidence_fanout", "128"}, {"max_label_cardinality", "256"}}) {
    auto m = defaults;
    m[key] = value;
    const auto tags = MapTags(m);
    Check(!c::NativeAgentConfigFromTags<R>(tags).ok(), "Q73 key refused without the policy record: " + key);
    const auto agreed = c::NativeAgentConfigFromTags<R>(tags, &policy);
    Check(agreed.ok() && *agreed.record == *plain.record, "Q73 key equal to the policy record: accepted, not carried: " + key);
    auto other = m;
    other[key] = "7";
    Check(!c::NativeAgentConfigFromTags<R>(MapTags(other), &policy).ok(), "Q73 key disagreeing with the policy refused: " + key);
    other[key] = "x";
    Check(!c::NativeAgentConfigFromTags<R>(MapTags(other), &policy).ok(), "Q73 key not a number refused: " + key);
    auto twice = tags;
    for (const auto& t : tags)
      if (t.key == key) twice.push_back(t);
    Check(!c::NativeAgentConfigFromTags<R>(twice, &policy).ok(), "Q73 key repeated refused: " + key);
  }
  for (const auto& d : c::NativeAgentConfigCommonFields())
    Check(d.key != "max_history_query_rows" && d.key != "max_evidence_fanout" && d.key != "max_label_cardinality" &&
              d.tag >= 64 && d.tag <= 73,
          "Q73: not a common config field");
}

// ---- every family: generated tables vs the schema map, defaults round trip ----
// 3c (owner Q71 = 1): formerly the exact unrepresented set; now each is a
// one-value enum {baseline} and nothing is unrepresented.
const std::map<std::string, std::set<std::string>> kExpectedBaselineOnly{
    {"admission_control_policy", {"workload_class_rules"}},
    {"parser_health_policy", {"package_quarantine_rules"}},
    {"backup_policy", {"blocker_policy"}},
    {"identity_lifecycle_policy", {"auth_anomaly_policy", "redaction_class"}},
    {"session_control_policy", {"self_view_redaction"}},
    {"alerting_baseline", {"grouping_rules"}},
    {"upgrade_policy", {"rolling_step_order", "rollback_gate"}},
    {"filespace_capacity_policy", {"required_approval_class"}},
    {"page_preallocation_policy", {"allowed_page_families", "workload_calendar"}},
    {"page_relocation_policy", {"unmovable_blocker_classes"}}};
void EveryFamily() {
  unsigned families = 0, unrepresented = 0, baseline_only = 0;
  std::set<p::u32> schema_ids;
  for (const auto& identity : c::kCatalogAgentPolicyFamilies) {
    const std::string tag(identity.tag);
    const auto fields = c::NativeAgentConfigFieldsFor(identity.family);
    const auto common = c::NativeAgentConfigCommonFields();
    const auto schema = a::AgentPolicySchemaForFamily(tag);
    // 3c (owner Q73 = 1): the schema map's last three common fields are
    // policy-record fields; they keep reserved tags 74..76 and no descriptor.
    const std::set<std::string> policy_record{"max_history_query_rows", "max_evidence_fanout", "max_label_cardinality"};
    std::vector<a::AgentPolicyFieldSchema> config_schema;
    for (std::size_t i = 0; i < schema.size(); ++i)
      if (!(i >= fields.size() && policy_record.count(schema[i].name))) config_schema.push_back(schema[i]);
      else Check(i - fields.size() + 64 >= 74, "Q73 fields are the last three common fields");
    Check(config_schema.size() + 3 == schema.size(), "three common fields are policy-record only: " + tag);
    Check(!fields.empty() && fields.size() + common.size() == config_schema.size(), "generated table size = schema map: " + tag);
    const auto expected_baseline = kExpectedBaselineOnly.find(tag);
    for (std::size_t i = 0; i < config_schema.size(); ++i) {
      const auto& d = i < fields.size() ? fields[i] : common[i - fields.size()];
      const auto& s = config_schema[i];
      Check(d.key == s.name && d.required == s.required &&
                a::AgentPolicyFieldTypeName(s.type) == SourceTypeName(d.source_type) &&
                d.has_minimum == s.minimum.has_value() && (!d.has_minimum || d.minimum == *s.minimum) &&
                d.has_maximum == s.maximum.has_value() && (!d.has_maximum || d.maximum == *s.maximum),
            "generated field = schema map field: " + tag + "." + s.name);
      Check(d.tag == (i < fields.size() ? 4 + i : 64 + (i - fields.size())), "field tag layout: " + s.name);
      if (!s.allowed_values.empty())
        Check(d.kind == c::NativeAgentConfigValueKind::enumeration, "allowed values become an enumeration: " + s.name);
      const bool is_baseline = expected_baseline != kExpectedBaselineOnly.end() && expected_baseline->second.count(s.name);
      if (is_baseline) {  // 3c (owner Q71 = 1)
        Check(d.kind == c::NativeAgentConfigValueKind::enumeration && s.allowed_values.empty() &&
                  s.default_value == "baseline" &&
                  (s.type == a::AgentPolicyFieldType::token || s.type == a::AgentPolicyFieldType::token_list),
              "free-text baseline field is a one-value enum: " + s.name);
        ++baseline_only;
      }
      if (s.type == a::AgentPolicyFieldType::decimal) Check(d.kind == c::NativeAgentConfigValueKind::decimal, "decimal: " + s.name);
    }
    std::set<std::string> listed;
    for (const auto key : c::NativeAgentConfigUnrepresentedKeys(identity.family)) listed.emplace(key);
    for (const auto& d : fields)
      Check(listed.count(std::string(d.key)) == (d.kind == c::NativeAgentConfigValueKind::unrepresented),
            "unrepresented list = unrepresented descriptors");
    Check(listed.empty(), "3c (owner Q71 = 1): nothing unrepresented: " + tag);
    unrepresented += listed.size();
    const bool dispatched = c::WithNativeAgentConfigType(identity.family, [&](auto type) {
      using R = typename decltype(type)::type;
      Check(R::family == identity.family && R::schema_id == c::NativeAgentConfigSchemaIdFor(identity.family),
            "family type and schema id");
      schema_ids.insert(R::schema_id);
      auto r = FromDefaults<R>();
      const auto raw = Roundtrip(r);
      Check(c::IsNativeAgentPayload(raw), "config payload selected");
      FullCommon(r);
      Roundtrip(r);
      // The adapter refuses a missing, repeated or unknown key and a bad boolean spelling.
      auto defaults = a::DefaultAgentPolicyConfigFieldsForFamily(tag);
      auto tags = MapTags(defaults);
      auto missing = tags;
      missing.pop_back();
      Check(!c::NativeAgentConfigFromTags<R>(missing).ok(), "missing config key refused");
      auto repeated = tags;
      repeated.push_back(tags.front());
      Check(!c::NativeAgentConfigFromTags<R>(repeated).ok(), "repeated config key refused");
      auto unknown = tags;
      unknown.push_back({"no_such_key", "1"});
      Check(!c::NativeAgentConfigFromTags<R>(unknown).ok(), "unknown config key refused");
      auto with_common = tags;
      with_common.push_back({"max_thread_slots", "1"});
      with_common.push_back({"protect_foreground_work", "true"});
      const auto common_adapted = c::NativeAgentConfigFromTags<R>(with_common);
      Check(common_adapted.ok() && common_adapted.record->common.max_thread_slots == 1u &&
                common_adapted.record->common.protect_foreground_work == true,
            "common optional keys adapted");
      // 3c (owner Q71 = 1): a baseline field accepts exactly "baseline".
      if (expected_baseline != kExpectedBaselineOnly.end())
        for (const auto& key : expected_baseline->second)
          for (const std::string_view bad_value : {"other", "", "baseline,baseline", "Baseline", "baseline "}) {
            auto bad = tags;
            for (auto& t : bad)
              if (t.key == key) t.value = bad_value;
            Check(!c::NativeAgentConfigFromTags<R>(bad).ok(), "baseline field refuses other text: " + key);
          }
      for (const auto& d : fields) {
        if (d.kind != c::NativeAgentConfigValueKind::boolean) continue;
        auto bad = tags;
        for (auto& t : bad)
          if (t.key == d.key) t.value = "1";  // config booleans are spelled true/false
        Check(!c::NativeAgentConfigFromTags<R>(bad).ok(), "config boolean spelled 1 refused");
      }
      ++families;
    });
    Check(dispatched, "every family has a config record type");
  }
  Check(families == 36 && schema_ids.size() == 36 && *schema_ids.begin() == 65642 && *schema_ids.rbegin() == 65677,
        "36 config schemas 65642..65677");
  Check(unrepresented == 0 && baseline_only == 13, "3c (owner Q71 = 1): 13 one-value baseline enums, none unrepresented");
  Check(!c::WithNativeAgentConfigType(c::CatalogAgentPolicyFamily::none, [](auto) {}) &&
            !c::WithNativeAgentConfigType(c::CatalogAgentPolicyFamily(37), [](auto) {}),
        "no config type for an unlisted family");
  Check(c::NativeAgentConfigSchemaIdFor(c::CatalogAgentPolicyFamily::none) == 0, "no schema id for none");
  // Adapter value errors on the coordinator.
  using R = c::NativeAgentConfig_agent_coordinator_policy;
  const auto defaults = a::DefaultAgentPolicyConfigFieldsForFamily("agent_coordinator_policy");
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"managed_layers", "L1,,L3"}, {"managed_layers", "L1,L2,"}, {"managed_layers", "l1_observation"},
           {"startup_verification_class", "startup_partial_verify"}, {"startup_agent", "no_agent"},
           {"max_agents_per_cycle", "-1"}, {"max_agents_per_cycle", "1x"}, {"startup_clock_fail_on_rollback", "1"}}) {
    auto m = defaults;
    m[key] = value;
    Check(!c::NativeAgentConfigFromTags<R>(MapTags(m)).ok(), "coordinator adapter refuses " + key + "=" + value);
  }
  auto m = defaults;
  m["managed_layers"] = "L1,L2,L1";
  const auto dup = c::NativeAgentConfigFromTags<R>(MapTags(m));
  Check(dup.ok(), "the adapter maps; the codec refuses");
  auto r = *dup.record;
  SetHead(r, 640);
  Refuse(r);
}

// ---- independent render-back: record -> the text the builder wrote ----
// A test-side key table (not the codec's), so a swapped field in the
// hand-written adapters fails here even though it would round-trip.
template <class En, std::size_t N>
std::string TagOf(const std::array<c::CatalogAgentTagEntry<En>, N>& table, En v) {
  for (const auto& e : table)
    if (e.value == v) return std::string(e.tag);
  return "?";
}
std::string TypeTag(c::CatalogAgentType v) {
  for (const auto& e : c::kCatalogAgentTypes)
    if (e.type == v) return std::string(e.tag);
  return "?";
}
std::string MetricTag(c::CatalogMetricFamily v) {
  for (const auto& e : c::kCatalogMetricFamilies)
    if (e.family == v) return std::string(e.tag);
  return "?";
}
template <class L, class F>
std::string Join(const L& list, F tag) {
  std::string out;
  for (const auto v : list.values()) out += (out.empty() ? "" : ",") + tag(v);
  return out;
}
std::map<std::string, std::string> Render(const c::NativeAgentPolicyRecord& r) {
  const auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
  std::map<std::string, std::string> m{
      {"policy_class", TagOf(c::kCatalogAgentPolicyClassTags, r.policy_class)},
      {"policy_family", FamilyTag(r.policy_family)},
      {"policy_generation", std::to_string(r.policy_generation)},
      {"agent_type", TypeTag(r.agent_type)},
      {"layer", TagOf(c::kCatalogAgentRuntimeLayerTags, r.layer)},
      {"deployment", TagOf(c::kCatalogAgentDeploymentTags, r.deployment)},
      {"authority", TagOf(c::kCatalogAgentAuthorityClassTags, r.authority)},
      {"enabled", flag(r.enabled)},
      {"activation", TagOf(c::kCatalogAgentActivationProfileTags, r.activation)},
      {"action_mode", TagOf(c::kCatalogAgentActionModeTags, r.action_mode)},
      {"invalid_policy_behavior", TagOf(c::kCatalogAgentInvalidPolicyBehaviorTags, r.invalid_policy_behavior)},
      {"allow_live_action", flag(r.allow_live_action)},
      {"require_manual_approval", flag(r.require_manual_approval)},
      {"require_dry_run_before_live", flag(r.require_dry_run_before_live)},
      {"evidence_required", flag(r.evidence_required)},
      {"explainability_required", flag(r.explainability_required)},
      {"editable_seed_policy", flag(r.editable_seed_policy)},
      {"run_interval_microseconds", std::to_string(r.run_interval_microseconds)},
      {"jitter_microseconds", std::to_string(r.jitter_microseconds)},
      {"lease_microseconds", std::to_string(r.lease_microseconds)},
      {"cooldown_microseconds", std::to_string(r.cooldown_microseconds)},
      {"max_runtime_microseconds", std::to_string(r.max_runtime_microseconds)},
      {"max_restart_attempts", std::to_string(r.max_restart_attempts)},
      {"initial_backoff_microseconds", std::to_string(r.initial_backoff_microseconds)},
      {"max_backoff_microseconds", std::to_string(r.max_backoff_microseconds)},
      {"max_history_query_rows", std::to_string(r.max_history_query_rows)},
      {"max_evidence_fanout", std::to_string(r.max_evidence_fanout)},
      {"max_label_cardinality", std::to_string(r.max_label_cardinality)},
      {"action_budget_per_window", std::to_string(r.action_budget_per_window)},
      {"required_metric_families", Join(r.required_metric_families, MetricTag)},
      {"policy_dependencies", Join(r.policy_dependencies, FamilyTag)}};
  // scope: the bit set holds exactly the path's segments (segment order is
  // not stored; it is a set).
  std::string scope;
  for (const auto& e : c::kCatalogAgentScopeTags)
    if ((r.scope & e.value) != c::CatalogAgentScope::none) scope += (scope.empty() ? "" : "/") + std::string(e.tag);
  m["scope"] = scope;
  return m;
}
std::map<std::string, std::string> Render(const c::NativeAgentRegistrationRecord& r) {
  return {{"agent_type", TypeTag(r.agent_type)},
          {"policy_families", Join(r.policy_families, FamilyTag)},
          {"state", TagOf(c::kCatalogAgentRegistrationStateTags, r.state)},
          {"managed_by", TagOf(c::kCatalogAgentManagedByTags, r.managed_by)},
          {"launch_trigger", TagOf(c::kCatalogAgentLaunchTriggerTags, r.launch_trigger)},
          {"starts_during_create", r.starts_during_create ? "1" : "0"}};
}
std::string Text(bool v) { return v ? "true" : "false"; }
std::map<std::string, std::string> Render(const c::NativeAgentConfig_agent_coordinator_policy& r) {
  return {{"startup_launch_enabled", Text(r.startup_launch_enabled)},
          {"startup_verification_class",
           TagOf(c::kNativeAgentConfigEnum_agent_coordinator_policy_startup_verification_classTags,
                 r.startup_verification_class)},
          {"startup_clock_max_backward_ns", std::to_string(r.startup_clock_max_backward_ns)},
          {"startup_clock_max_forward_jump_ns", std::to_string(r.startup_clock_max_forward_jump_ns)},
          {"startup_clock_fail_on_rollback", Text(r.startup_clock_fail_on_rollback)},
          {"startup_clock_fail_on_monotonic_regression", Text(r.startup_clock_fail_on_monotonic_regression)},
          {"startup_clock_fail_on_forward_jump", Text(r.startup_clock_fail_on_forward_jump)},
          {"startup_uuid_max_regression_ms", std::to_string(r.startup_uuid_max_regression_ms)},
          {"startup_uuid_same_ms_wait_timeout_ms", std::to_string(r.startup_uuid_same_ms_wait_timeout_ms)},
          {"go_no_go_required", Text(r.go_no_go_required)},
          {"managed_layers", Join(r.managed_layers, [](auto v) { return TagOf(c::kNativeAgentManagedLayerTags, v); })},
          {"worker_launch_forbidden", Text(r.worker_launch_forbidden)},
          {"max_agents_per_cycle", std::to_string(r.max_agents_per_cycle)},
          {"max_schedule_queue_depth", std::to_string(r.max_schedule_queue_depth)},
          {"schedule_interval_microseconds", std::to_string(r.schedule_interval_microseconds)},
          {"startup_agent", TypeTag(r.startup_agent)},
          {"shutdown_agent", TypeTag(r.shutdown_agent)},
          {"coordinator_stops_last", Text(r.coordinator_stops_last)}};
}
std::map<std::string, std::string> Render(const c::NativeAgentConfig_agent_startup_policy& r) {
  return {{"launch_trigger", TagOf(c::kCatalogAgentLaunchTriggerTags, r.launch_trigger)},
          {"engine_verification_required", Text(r.engine_verification_required)},
          {"committed_policy_required", Text(r.committed_policy_required)},
          {"child_readiness_required", Text(r.child_readiness_required)},
          {"configured_components_only", Text(r.configured_components_only)},
          {"dependency_order", Join(r.dependency_order, TypeTag)},
          {"readiness_timeout_microseconds", std::to_string(r.readiness_timeout_microseconds)},
          {"max_retry_attempts", std::to_string(r.max_retry_attempts)}};
}
template <class R>
void RenderMatches(const R& r, const std::vector<c::NativeAgentPropertyTag>& tags, std::set<std::string> skip,
                   const std::string& what) {
  const auto rendered = Render(r);
  std::set<std::string> compared;
  for (const auto& t : tags) {
    const std::string key(t.key);
    if (skip.count(key)) continue;
    const auto found = rendered.find(key);
    Check(found != rendered.end(), what + ": no rendering for " + key);
    if (key == "scope") {  // a set: same segments, any order
      std::set<std::string> want, got;
      for (std::string_view v = t.value; !v.empty();) {
        const auto end = v.find('/');
        want.emplace(v.substr(0, end));
        v.remove_prefix(end == v.npos ? v.size() : end + 1);
      }
      for (std::string_view v = found->second; !v.empty();) {
        const auto end = v.find('/');
        got.emplace(v.substr(0, end));
        v.remove_prefix(end == v.npos ? v.size() : end + 1);
      }
      Check(want == got, what + ": scope segments");
    } else {
      Check(found->second == t.value, what + ": " + key + " landed in its own field (" + found->second +
                                          " vs " + std::string(t.value) + ")");
    }
    compared.insert(key);
  }
  Check(compared.size() == rendered.size(), what + ": every rendered field compared");
}

// ---- every row DefaultAgentBootstrapDeclarations writes today ----
void BuilderRows() {
  const auto declarations = a::DefaultAgentBootstrapDeclarations();
  unsigned policies = 0, registrations = 0, n = 0;
  std::set<std::string> config_families;
  for (const auto& d : declarations) {
    ++n;
    std::vector<std::string> keep;
    const auto top = Tags(d.properties, false, keep);
    const auto type = c::CatalogAgentTypeForTag(d.agent_type);
    Check(type.has_value(), "declared agent type has a constant: " + d.agent_type);
    if (d.kind == c::AgentBootstrapDefinitionKind::policy) {
      const auto adapted = c::NativeAgentPolicyFromTags(top);
      Check(adapted.ok(), "policy row representable: " + d.agent_type + " key=" + std::string(adapted.bad_key));
      auto r = *adapted.record;
      Check(r.agent_type == *type, "policy agent_type property = definition agent_type");
      RenderMatches(r, top, {"policy_name"}, "policy row " + d.agent_type);
      r.definition_uuid = r.policy_uuid = f::Id(10000 + n);
      r.database_uuid = f::Id(7);
      r.origin_transaction_uuid = Tx(5);
      r.origin_local_transaction_id = 1;
      r.generation = r.policy_generation;
      Roundtrip(r);
      // The family config record from the same row's config.* keys.
      const auto config = Tags(d.properties, true, keep);
      // 3c (owner Q73 = 1): the builder never writes the three policy-record
      // budgets under config.
      for (const auto& t : config)
        Check(t.key != "max_history_query_rows" && t.key != "max_evidence_fanout" && t.key != "max_label_cardinality",
              "builder writes no Q73 budget under config");
      const std::string family_tag = FamilyTag(r.policy_family);
      config_families.insert(family_tag);
      const bool dispatched = c::WithNativeAgentConfigType(r.policy_family, [&](auto t) {
        using R = typename decltype(t)::type;
        const auto cfg = c::NativeAgentConfigFromTags<R>(config);
        Check(cfg.ok(), "config row representable: " + family_tag + " key=" + std::string(cfg.bad_key));
        auto record = *cfg.record;
        if constexpr (std::is_same_v<R, c::NativeAgentConfig_agent_coordinator_policy> ||
                      std::is_same_v<R, c::NativeAgentConfig_agent_startup_policy>)
          RenderMatches(record, config, {}, "config row " + family_tag);
        record.definition_uuid = r.definition_uuid;
        record.origin_transaction_uuid = r.origin_transaction_uuid;
        record.origin_local_transaction_id = 1;
        Roundtrip(record);
      });
      Check(dispatched, "policy family has a config record");
      // Dropped text keys: only policy_name (names table) is not carried.
      auto no_name = top;
      std::erase_if(no_name, [](const auto& t) { return t.key == "policy_name"; });
      Check(!c::NativeAgentPolicyFromTags(no_name).ok(), "policy_name still required at the boundary");
      ++policies;
    } else {
      const auto adapted = c::NativeAgentRegistrationFromTags(top);
      Check(adapted.ok(), "registration row representable: " + d.agent_type + " key=" + std::string(adapted.bad_key));
      auto r = *adapted.record;
      Check(r.agent_type == *type && r.policy_families.count >= 1, "registration agent type and families");
      RenderMatches(r, top, {"profile_name", "profile_class"}, "registration row " + d.agent_type);
      r.definition_uuid = f::Id(20000 + n);
      r.policy_uuid = f::Id(10000 + 1 + d.primary_policy_index);
      r.database_uuid = f::Id(7);
      r.origin_transaction_uuid = Tx(5);
      r.origin_local_transaction_id = 1;
      r.generation = 1;
      Roundtrip(r);
      ++registrations;
    }
  }
  Check(registrations == 32 && policies >= registrations, "every agent's rows representable");
  // Two schema-map families have no agent requiring them today, so no builder
  // row; their default configs are still round-tripped by EveryFamily().
  std::set<std::string> unwritten;
  for (const auto& identity : c::kCatalogAgentPolicyFamilies)
    if (!config_families.count(std::string(identity.tag))) unwritten.emplace(identity.tag);
  Check(config_families.size() == 34 &&
            unwritten == std::set<std::string>{"coordinator_policy", "cleanup_cycle_policy"},
        "builder writes config for 34 families (all but coordinator_policy, cleanup_cycle_policy)");
  std::cout << "builder rows: " << policies << " policies, " << registrations << " registrations\n";
}
void PolicyAdapterRefusals() {
  const auto declarations = a::DefaultAgentBootstrapDeclarations();
  std::vector<std::string> keep;
  const auto policy = std::find_if(declarations.begin(), declarations.end(), [](const auto& d) {
    return d.kind == c::AgentBootstrapDefinitionKind::policy && d.agent_type == "agent_coordinator";
  });
  const auto registration = std::find_if(declarations.begin(), declarations.end(), [](const auto& d) {
    return d.kind == c::AgentBootstrapDefinitionKind::registration;
  });
  const auto top = Tags(policy->properties, false, keep);
  Check(c::NativeAgentPolicyFromTags(top).ok(), "coordinator policy row");
  for (const auto& [key, value] : std::vector<std::pair<std::string_view, std::string_view>>{
           {"enabled", "true"}, {"scope", "node/no_such_scope"}, {"scope", "node/node"}, {"scope", ""},
           {"layer", "L1"}, {"invalid_policy_behavior", "made up"}, {"required_metric_families", "no_metric"},
           {"policy_dependencies", "no_family,"}, {"run_interval_microseconds", "+1"}, {"agent_type", "nobody"},
           {"action_mode", "live"}, {"policy_name", ""}}) {
    auto bad = top;
    for (auto& t : bad)
      if (t.key == key) t.value = value;
    Check(!c::NativeAgentPolicyFromTags(bad).ok(), "policy adapter refuses " + std::string(key));
  }
  auto extra = top;
  extra.push_back({"profile_name", "x"});
  Check(!c::NativeAgentPolicyFromTags(extra).ok(), "unknown policy key refused");
  auto repeated = top;
  repeated.push_back(top[3]);
  Check(!c::NativeAgentPolicyFromTags(repeated).ok(), "repeated policy key refused");
  const auto reg = Tags(registration->properties, false, keep);
  for (const auto& [key, value] : std::vector<std::pair<std::string_view, std::string_view>>{
           {"profile_class", "agent_policy"}, {"starts_during_create", "false"}, {"state", "on"},
           {"policy_families", ""}, {"managed_by", "nobody"}, {"launch_trigger", "now"}}) {
    auto bad = reg;
    for (auto& t : bad)
      if (t.key == key) t.value = value;
    const auto adapted = c::NativeAgentRegistrationFromTags(bad);
    bool refused = !adapted.ok();
    if (!refused) {  // empty policy_families maps; the codec refuses it
      auto r = *adapted.record;
      r.definition_uuid = f::Id(1); r.policy_uuid = f::Id(2); r.database_uuid = f::Id(7);
      r.origin_transaction_uuid = Tx(5); r.origin_local_transaction_id = 1; r.generation = 1;
      refused = !c::NativeAgentRegistrationEncodedBytes(r);
    }
    Check(refused, "registration adapter/codec refuses " + std::string(key));
  }
}
}  // namespace

int main() {
  try {
    Policies();
    Registrations();
    Coordinator();
    StartupConfig();
    RecommendationConfig();
    BaselineOnly();
    PolicyRecordBudgets();
    EveryFamily();
    BuilderRows();
    PolicyAdapterRefusals();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  std::cout << "catalog_native_agent_record: " << checks << " checks passed\n";
  return 0;
}
