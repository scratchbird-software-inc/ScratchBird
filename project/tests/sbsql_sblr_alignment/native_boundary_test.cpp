// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_BOUNDARY (thin-slice S5; owner Q146 / Q150 / Q168 / Q169, plan 36 T7; controller 2026-12-09).
// COMMIT / ROLLBACK on a native session: the REAL server step (srv::RunNativeTransactionBoundary, the function the server's execute path calls
// for a canonical commit or rollback on a native session) on production-created single-file databases, with the real HandleAuthHandoff session,
// a native receipt for the REPLACEMENT transaction and `SELECT 1 AS one` / `SELECT 2 AS two` through the real SBsql parser in it.
// States (one child process each, per page size): boundary (every behaviour), crasha_<pre>_<k> / crashb_<n>_<m>_<c> (a failed or crashed boundary and the
// next open), trace (the per-boundary write and sync counts, read by native_boundary_strace.py). Reuses native_select_one_test.cpp's helpers.
#define SB_NATIVE_SELECT_ONE_FIXTURE_ONLY 1
#include "native_select_one_test.cpp"

#include "server/sblr_dispatch_server.hpp"

#include <thread>

namespace {
namespace pr = scratchbird::wire::public_result;

// ---- the real step -------------------------------------------------------------------------------------------------------------------
struct Step {
  bool ok = false;
  std::string code, detail, payload;
  srv::ServerTransactionState replacement;
};
Step Boundary(Approved& a, bool commit, bool retaining = false, const srv::ServerSessionRecord* record_override = nullptr) {
  srv::ServerTransactionState finalized;
  finalized.local_transaction_id = a.approval.transaction_order;
  finalized.transaction_uuid = a.approval.transaction_uuid;
  const auto out = srv::RunNativeTransactionBoundary(record_override ? *record_override : *a.record, finalized, commit, retaining);
  Step step;
  step.ok = out.ok;
  step.code = out.diagnostic_code;
  step.detail = out.detail;
  step.payload = out.payload;
  step.replacement = out.replacement;
  if (out.ok) {
    a.approval.transaction_order = out.replacement.local_transaction_id;
    a.approval.transaction_uuid = out.replacement.transaction_uuid;
  }
  return step;
}
struct BoundaryVector {
  bool ok = false;
  std::string operation_id, finality;
  std::uint64_t ended_order = 0, new_order = 0;
  p::Uuid ended_uuid, new_uuid;
  std::size_t fields = 0;
};
BoundaryVector Decode(const std::string& payload) {
  BoundaryVector out;
  std::vector<pr::Field> f;
  if (!pr::Decode(payload, &f) || f.size() != 7) return out;
  out.fields = f.size();
  if (f[0].name != "operation_id" || f[1].name != "result_kind" || f[1].value != "native_transaction_boundary" || f[2].name != "ended_order" ||
      f[3].name != "ended_uuid" || f[4].name != "finality" || f[5].name != "new_order" || f[6].name != "new_uuid")
    return out;
  out.operation_id = f[0].value;
  out.ended_order = LE64(f[2].value);
  out.ended_uuid = UuidOf(f[3].value);
  out.finality = f[4].value;
  out.new_order = LE64(f[5].value);
  out.new_uuid = UuidOf(f[6].value);
  out.ok = true;
  return out;
}
off::FileView File() { return off::ReadFile(served.path, served.facts->page_size_bytes); }

// Releases the receipt, acquires one for the session's CURRENT transaction (the replacement), runs one SELECT in it.
std::int64_t SelectIn(Approved& a, PublicSession& ps, const std::string& sql) {
  const auto generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto context = ContextFor(a.record->database_path, a.record->effective_user_uuid, a.record->session_uuid, a.approval.transaction_uuid, a.approval.transaction_order, generation);
  auto receipt = Acquire(ps, context, a.approval.transaction_uuid);
  Check(receipt.ok(), "a native receipt for the replacement transaction: " + receipt.code);
  const auto got = Run(sql, ps, receipt, a);
  Check(got.status == SB_ENGINE_STATUS_OK && got.vector.ok && got.vector.values.size() == 1, "'" + sql + "' runs in the replacement transaction: " + got.refusal.code);
  Release(receipt);
  return got.vector.values.empty() ? -1 : got.vector.values[0];
}

// A FailAfterWritesSink as the S4d test's: the first `good` sector writes reach the file, the rest fail with EIO.
class FailAfter final : public d::CarefulWriteSink {
 public:
  FailAfter(d::CarefulWriteSink& inner, int good) : inner_(inner), good_(good) {}
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    if (good_-- > 0) return inner_.WriteAt(offset, bytes);
    d::BoundedIoResult r;
    r.operation = d::BoundedIoOperation::write;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  d::CarefulWriteSink& inner_;
  int good_;
};

const char* StateName(pgs::TipStateCode code) {
  static const char* const names[] = {"unused", "starting", "active", "read_only_active", "prepared", "committing", "committed",
                                      "rolling_back", "rolled_back", "dead", "limbo", "recovering", "archived"};
  const auto i = static_cast<unsigned>(code);
  return i < 13 ? names[i] : "?";
}

// ---- THE STATE: every behaviour ---------------------------------------------------------------------------------------------------
void BoundaryState() {
  Mark("approve");
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  const auto n0 = s1->approval.transaction_order;
  const auto uuid0 = s1->approval.transaction_uuid;
  const auto ledger_before = File().ledger.size();

  // ---- (a) COMMIT: the vector, the file, a receipt for the replacement, SELECT 1 AS one = 1 ----
  const auto generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto old_receipt = Acquire(*ps1, ContextFor(*s1, generation), uuid0);
  Check(old_receipt.ok(), "a receipt for the first transaction");
  const auto step = Boundary(*s1, true);
  Check(step.ok, "COMMIT ends the transaction: " + step.code + " " + step.detail);
  const auto vec = Decode(step.payload);
  Check(vec.ok && vec.fields == 7, "the result is the 7-field boundary message vector (no prose, no name)");
  Check(vec.operation_id == "engine.op.txn_commit" && vec.finality == "committed" && vec.ended_order == n0 && vec.ended_uuid == uuid0 &&
            vec.new_order == n0 + 1 && vec.new_uuid == s1->approval.transaction_uuid && !(vec.new_uuid == uuid0),
        "the vector carries the ended order and UUID, the finality and the NEW order (one more) and UUID");
  {
    const auto file = File();
    Check(file.words.at(n0).state == pgs::TipStateCode::committed, "TIP: the ended slot is committed");
    Check(file.words.at(n0 + 1).state == pgs::TipStateCode::active && file.counters.next_transaction == n0 + 2, "TIP: the replacement slot is active and Next advanced by exactly one");
    const auto& h0 = file.headers.at(n0);
    const auto& h1 = file.headers.at(n0 + 1);
    Check(h0.final_state == pgs::TipStateCode::committed && h0.completed_at_utc_ns != 0 && h0.commit_time_utc_ns != 0, "header of the ended: final, with commit and completion times");
    Check(h1.principal_or_agent_uuid == h0.principal_or_agent_uuid && h1.attachment_uuid == h0.attachment_uuid && h1.session_uuid == h0.session_uuid &&
              !h1.session_uuid.is_nil() && h1.isolation_profile == h0.isolation_profile && h1.read_only == h0.read_only,
          "header of the replacement: +64 principal, +80 attachment, +96 session and the settings are the session's, unchanged");
    Check(file.ledger.size() == ledger_before, "the ledger is unchanged: a boundary appends no record");
  }
  // (c) the receipt of the ended transaction cannot dispatch; the old id is refused as an identity
  {
    const FileEvidence before;
    // FINDING (reported, not hidden): the ENGINE does not revalidate a HELD receipt against the TIP at dispatch (a receipt is an immutable
    // in-memory view); the barrier is the server's session map (the ended transaction is removed, so an execute naming it gets
    // TRANSACTION_SELECTOR_MISMATCH) and the receipt SOURCE (a new acquire for the ended transaction is refused, below).
    const auto lookup = served.loop->SubmitReceiptLookup(s1->session(), uuid0).get();
    Check(!lookup.ok(), "the loop's receipt lookup refuses the ended transaction (not active)");
    const auto stale = Acquire(*ps1, ContextFor(s1->record->database_path, s1->record->effective_user_uuid, s1->record->session_uuid, uuid0, n0, generation), uuid0);
    Check(!stale.ok() && !stale.code.empty(), "the ended transaction's id is refused as a transaction identity: " + stale.code);
    CheckUntouched(before, "the refusals");
  }
  Release(old_receipt);
  Check(SelectIn(*s1, *ps1, "SELECT 1 AS one") == 1, "SELECT 1 AS one = 1 in the replacement");
  // the replacement's begin view (S4f: captured BEFORE the write): the loop's receipt lookup shows it, with the reader = this transaction
  {
    const auto input = served.loop->SubmitReceiptLookup(s1->session(), s1->approval.transaction_uuid).get();
    Check(input.ok() && input.snapshot_is_begin_capture && input.snapshot != nullptr, "the replacement has its begin view captured in memory");
    Check(input.snapshot->reader_transaction.value == s1->approval.transaction_order, "the begin view's reader is the replacement");
    const auto& excluded = input.snapshot->active_excluded_local_transaction_ids;
    Check(std::find(excluded.begin(), excluded.end(), n0) == excluded.end(), "the ended transaction is not in the replacement's in-progress exclusions (the view was taken before the write)");
  }

  // ---- ROLLBACK likewise ----
  const auto n1 = s1->approval.transaction_order;
  const auto rb = Boundary(*s1, false);
  Check(rb.ok, "ROLLBACK ends the transaction: " + rb.code);
  const auto rbv = Decode(rb.payload);
  Check(rbv.ok && rbv.operation_id == "engine.op.txn_rollback" && rbv.finality == "rolled_back" && rbv.ended_order == n1 && rbv.new_order == n1 + 1, "the rollback vector");
  Check(File().words.at(n1).state == pgs::TipStateCode::rolled_back && File().words.at(n1 + 1).state == pgs::TipStateCode::active, "TIP: rolled_back and the next active");
  Check(SelectIn(*s1, *ps1, "SELECT 2 AS two") == 2, "SELECT 2 AS two = 2 after ROLLBACK");

  // ---- RETAINING: refused with the one specific code, nothing written, the session stays in its transaction ----
  {
    const FileEvidence before;
    const auto retain = Boundary(*s1, true, true);
    Check(!retain.ok && retain.code == "ENGINE.NATIVE.OPERATION_NOT_BUILT" && retain.detail == "requires=native_retaining_boundary", "RETAINING: ENGINE.NATIVE.OPERATION_NOT_BUILT requires=native_retaining_boundary");
    CheckUntouched(before, "the retaining refusal");
    Check(SelectIn(*s1, *ps1, "SELECT 1 AS one") == 1, "the session is still in its transaction after the refusal");
  }

  // ---- settings: the replacement takes the session's CURRENT settings, not the finished transaction's (Q168) ----
  {
    auto read_only_session = *s1->record;
    read_only_session.default_transaction_read_only = true;
    const auto ro = Boundary(*s1, true, false, &read_only_session);
    Check(ro.ok && ro.replacement.read_only, "a session whose current setting is read-only gets a read-only replacement");
    Check(File().headers.at(s1->approval.transaction_order).read_only, "header: the replacement is read-only");
    const auto rw = Boundary(*s1, true);  // the finished transaction is read-only; the session's setting is read-write
    Check(rw.ok && !rw.replacement.read_only && !File().headers.at(s1->approval.transaction_order).read_only,
          "the next replacement is read-write: the session's setting, not the finished (read-only) transaction's");
  }

  // ---- repeated boundaries: exactly one number per boundary, chain growth across a page, the ledger unchanged, identities carried ----
  {
    auto before = File();
    const auto ledger_count = before.ledger.size();
    const auto header_pages = before.header_pages.size(), state_pages = before.state_pages.size();
    std::vector<p::Uuid> seen;
    std::uint64_t count = 0;
    bool grew = false;
    while ((count < 100 || !grew) && count < 6000) {
      const auto from = s1->approval.transaction_order;
      const auto r = Boundary(*s1, count % 3 != 0);
      Check(r.ok && s1->approval.transaction_order == from + 1, "boundary " + std::to_string(count) + " advanced the number by exactly one: " + r.code);
      seen.push_back(s1->approval.transaction_uuid);
      ++count;
      if (count % 8 == 0) {
        const auto now = File();
        grew = now.header_pages.size() > header_pages && now.state_pages.size() >= state_pages;
      }
    }
    Check(grew, "the header chain grew across a page boundary during " + std::to_string(count) + " boundaries");
    const auto after = File();
    Check(after.ledger.size() == ledger_count, "100+ boundaries appended no ledger record");
    std::set<std::vector<p::byte>> unique;
    for (const auto& id : seen) unique.insert(std::vector<p::byte>(id.bytes.begin(), id.bytes.end()));
    Check(unique.size() == seen.size(), "no transaction UUID was reused");
    const auto last = s1->approval.transaction_order;
    Check(after.counters.next_transaction == last + 1, "Next is one past the last replacement");
    for (auto order = n0; order < last; ++order) {
      const auto state = after.words.at(order).state;
      Check(state == pgs::TipStateCode::committed || state == pgs::TipStateCode::rolled_back, "slot " + std::to_string(order) + " is final");
      const auto& h = after.headers.at(order);
      const auto& next = after.headers.at(order + 1);
      Check(h.session_uuid == next.session_uuid && h.attachment_uuid == next.attachment_uuid && h.principal_or_agent_uuid == next.principal_or_agent_uuid,
            "slot " + std::to_string(order) + ": +64/+80/+96 carried to the replacement");
    }
    Check(after.words.at(last).state == pgs::TipStateCode::active, "the last replacement is active");
    Check(SelectIn(*s1, *ps1, "SELECT 1 AS one") == 1, "SELECT 1 AS one = 1 after the chain grew");
  }

  // ---- (d) two sessions, interleaved in threads: independent, distinct ids; (e) a foreign session cannot end another's transaction ----
  auto s2 = Approve();
  auto s3 = Approve();
  {
    const auto start2 = s2->approval.transaction_order, start3 = s3->approval.transaction_order;
    std::vector<std::vector<std::uint64_t>> orders(2);
    std::vector<std::vector<p::Uuid>> uuids(2);
    std::vector<int> failures(2, 0);
    std::vector<std::thread> threads;
    Approved* sessions[2] = {s2.get(), s3.get()};
    for (int t = 0; t < 2; ++t)
      threads.emplace_back([&, t] {
        for (int i = 0; i < 40; ++i) {
          const auto r = Boundary(*sessions[t], (i + t) % 2 == 0);
          if (!r.ok) { ++failures[t]; continue; }
          orders[t].push_back(r.replacement.local_transaction_id);
          uuids[t].push_back(r.replacement.transaction_uuid);
        }
      });
    for (auto& thread : threads) thread.join();
    Check(failures[0] == 0 && failures[1] == 0 && orders[0].size() == 40 && orders[1].size() == 40, "80 interleaved boundaries all succeeded");
    std::set<std::uint64_t> all;
    for (const auto& list : orders) all.insert(list.begin(), list.end());
    all.insert(start2);
    all.insert(start3);
    Check(all.size() == 82, "every transaction number is distinct across the two sessions (no reuse)");
    std::set<std::vector<p::byte>> ids;
    for (const auto& list : uuids) for (const auto& id : list) ids.insert(std::vector<p::byte>(id.bytes.begin(), id.bytes.end()));
    Check(ids.size() == 80, "every transaction UUID is distinct");
    const auto file = File();
    for (int t = 0; t < 2; ++t)
      for (const auto order : orders[t]) {
        Check(file.headers.at(order).session_uuid == sessions[t]->session(), "slot " + std::to_string(order) + " belongs to its own session in the header (+96)");
      }
    Check(!(sessions[0]->session() == sessions[1]->session()), "the two sessions differ");
    // (e) the foreign session: s3 tries to end s2's transaction through the same step
    const FileEvidence before;
    srv::ServerTransactionState others;
    others.local_transaction_id = s2->approval.transaction_order;
    others.transaction_uuid = s2->approval.transaction_uuid;
    const auto refused = srv::RunNativeTransactionBoundary(*s3->record, others, true, false);
    Check(!refused.ok && refused.diagnostic_code == "ENGINE.NATIVE.TRANSACTION_NOT_OWNER", "a foreign session cannot end another session's transaction: " + refused.diagnostic_code);
    CheckUntouched(before, "the foreign commit");
    Check(File().words.at(s2->approval.transaction_order).state == pgs::TipStateCode::active, "the other session's transaction is still active");
  }

  // ---- (f) disconnect after commits ends the session and rolls the REPLACEMENT back (S4e) ----
  {
    const auto replacement = s2->approval.transaction_order;
    ps1.reset();
    Disconnect(*s2);
    const auto file = File();
    Check(file.words.at(replacement).state == pgs::TipStateCode::rolled_back, "disconnect rolled the replacement back");
    const auto* ended = file.FindRecord(s2->session(), true);
    Check(ended != nullptr, "the session's ledger record exists");
    Disconnect(*s3);
    Disconnect(*s1);
    const auto end = File();
    for (const auto& [order, word] : end.words)
      Check(word.state != pgs::TipStateCode::active, "no slot is left active after every session ended (slot " + std::to_string(order) + ")");
  }
}

// ---- the crashed or failed boundary, then the next open ---------------------------------------------------------------------------
void CrashAState(const std::string& state) {
  int pre = 0, k = 0;
  std::sscanf(state.c_str(), "crasha_%d_%d", &pre, &k);
  auto s = Approve();
  for (int i = 0; i < pre; ++i) Check(Boundary(*s, true).ok, "a boundary before the fault");
  const auto n = s->approval.transaction_order;
  if (k >= 0) {
    srv::SetNativeTransactionSinkWrapperForTest(
        [k](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> { return std::make_unique<FailAfter>(inner, k); });
    const auto failed = Boundary(*s, true);
    srv::SetNativeTransactionSinkWrapperForTest({});
    Check(!failed.ok && failed.code == "ENGINE.NATIVE.TRANSACTION_WRITER_FAILED", "the client of the failed statement gets the writer-failed code: " + failed.code);
    Check(served.loop->serving_state() == srv::NativeServingState::failed, "the database is failed (requires reopen)");
    Check(Boundary(*s, false).code == "ENGINE.NATIVE.TRANSACTION_DATABASE_FAILED", "every later boundary is refused with the failed code");
  } else {
    Check(Boundary(*s, k == -1).ok, "the boundary completes (the crash comes after W5)");
  }
  std::printf("CRASH_STEP n=%llu m=%llu\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(n + 1));
  std::fflush(stdout);
  std::_Exit(0);  // a crash: no close, no session end, nothing flushed
}
void CrashBState(const std::string& state) {
  unsigned long long n = 0, m = 0;
  int committed = 0;
  std::sscanf(state.c_str(), "crashb_%llu_%llu_%d", &n, &m, &committed);
  std::optional<srv::NativeLoopServingFacts> facts;
  Check(served.loop->RunOnLoopThreadForTest([&] { facts = served.loop->FactsOnLoopThread(); }) && facts.has_value(), "the loop has its facts");
  Check(facts->serving_state == srv::NativeServingState::serving, "the database serves after the crash (no failed state)");
  Check(facts->ledger_orphaned_sessions == 1, "the crashed session is un-ended and counted orphaned: " + std::to_string(facts->ledger_orphaned_sessions));
  const auto file = File();
  const auto expected_n = committed == 1 ? pgs::TipStateCode::committed : committed == 2 ? pgs::TipStateCode::rolled_back : pgs::TipStateCode::dead;
  Check(file.words.at(n).state == expected_n, std::string("the ended transaction is ") + StateName(expected_n) + " (W5 " + (committed ? "landed" : "did not land") + "): " + StateName(file.words.at(n).state));
  if (file.words.count(m)) Check(file.words.at(m).state == pgs::TipStateCode::dead, "the replacement is dead: " + std::string(StateName(file.words.at(m).state)));
  Check(committed != 1 || file.headers.at(n).commit_time_utc_ns != 0, "a committed ended transaction has its final header");
  auto again = Approve();
  Check(again->approval.transaction_order == file.counters.next_transaction && again->approval.transaction_order > n, "the next login takes a fresh number");
  Check(Boundary(*again, true).ok, "a boundary works after the reopen");
  Disconnect(*again);
}

// ---- trace: per-boundary regions for native_boundary_strace.py ----------------------------------------------------------------------
void BoundaryTraceState() {
  Mark("approve");
  auto s = Approve();
  Mark("prepare");
  for (int i = 0; i < 8; ++i) {
    Mark("b" + std::to_string(s->approval.transaction_order));
    Check(Boundary(*s, i % 2 == 0).ok, "a traced boundary");
  }
  Mark("disconnect");
  Disconnect(*s);
}

std::optional<int> ExtraReceiptStateForTest(const std::string& state, const Offline&) {
  if (state == "boundary") { BoundaryState(); return 0; }
  if (state == "trace") { BoundaryTraceState(); return 0; }
  if (state.starts_with("crasha_")) { CrashAState(state); return 0; }
  if (state.starts_with("crashb_")) { CrashBState(state); return 0; }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    const auto size = static_cast<std::uint32_t>(std::stoul(argv[2]));
    RunState(self, artifacts.path(), size, "boundary");
    artifacts.Cleanup();
    std::printf("native_boundary: %d driver checks passed\n", checks);
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "crash") {
    // A failed (k >= 0: the k+1th sector write fails) or crashed (k = -1 commit, -2 rollback: a forced exit after the last write) boundary,
    // then the next open, for `pre` successful boundaries before it (pre 0: the completing order is even, 3 writes; pre 1: odd, 4 writes).
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    const auto size = static_cast<std::uint32_t>(std::stoul(argv[2]));
    const std::string material = SB_NATIVE_LOGIN_MATERIAL_TEST_BIN;
    int cases = 0;
    for (const auto& [pre, k] : std::vector<std::pair<int, int>>{{0, 0}, {0, 1}, {0, 2}, {0, -1}, {1, 0}, {1, 3}, {1, -2}}) {
      const auto dir = artifacts.path() / ("crash" + std::to_string(pre) + "_" + std::to_string(k + 2));
      fs::create_directory(dir);
      const auto path = dir / "crash.sdb";
      int status = 0;
      const auto created = Capture("'" + material + "' create '" + path.string() + "' " + std::to_string(size), &status);
      Check(status == 0 && created.starts_with("CREATED "), "create: " + created);
      const auto serve = [&](const std::string& st) {
        return Capture("'" + self + "' serve '" + path.string() + "' " + st + " " + Field(created, "database") + " " + Field(created, "bootstrap") + " " + Field(created, "service"), &status);
      };
      const auto first = serve("crasha_" + std::to_string(pre) + "_" + std::to_string(k));
      Check(status == 0 && first.find("CRASH_STEP") != std::string::npos, "the crashing child: " + first.substr(0, 2500));
      const auto n = std::stoull(Field(first, "n")), m = std::stoull(Field(first, "m"));
      const auto before = off::ReadFile(path, size);  // the file as the crash left it, before any open
      const bool committed = k < 0;  // the completion landed (W5): committed (k = -1) or rolled_back (k = -2)
      const auto landed = k == -1 ? pgs::TipStateCode::committed : pgs::TipStateCode::rolled_back;
      Check(before.words.at(n).state == (committed ? landed : pgs::TipStateCode::active),
            "after the crash (k=" + std::to_string(k) + "): the ended transaction " + std::to_string(n) + " is " + (committed ? "final (W5 landed)" : "still active, never committed") + ": " + StateName(before.words.at(n).state));
      if (before.words.count(m)) {
        const auto ms = before.words.at(m).state;
        Check(committed ? ms == pgs::TipStateCode::active : ms != pgs::TipStateCode::committed, "the replacement slot after the crash is never committed: " + std::string(StateName(ms)));
      }
      const auto second = serve("crashb_" + std::to_string(n) + "_" + std::to_string(m) + "_" + (k == -1 ? "1" : k == -2 ? "2" : "0"));
      Check(status == 0 && second.find("RECEIPT_OK") != std::string::npos, "the reopening child: " + second.substr(0, 2500));
      ++cases;
    }
    artifacts.Cleanup();
    std::printf("native_boundary_crash: %d crash cases, %d driver checks passed\n", cases, checks);
    return 0;
  }
  return 2;
}
