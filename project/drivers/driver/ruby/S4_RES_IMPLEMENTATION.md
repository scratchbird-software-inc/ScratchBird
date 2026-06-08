# DLB-RUBY-005 S4 RES Implementation

Date: 2026-03-04
Lane: `lanes/active/drivers/ruby`
Scope: resource lifecycle hardening plus deterministic resilience wiring tests.

## Changes Implemented

1. Idempotent client close semantics with full helper cleanup
   - File: `lib/scratchbird/client.rb`
   - Changes:
     - `Client#close` no longer exits early when `@socket` is nil.
     - Cleanup now always finalizes keepalive unregister/stop and leak guard release/detector stop.
     - Socket close errors (`IOError`/`SystemCallError`) are tolerated during teardown.
   - Effect:
     - Prevents residual keepalive/leak-tracker state when close is invoked from partial startup or error paths.

2. Connection close finalizes closed state under disconnect errors
   - File: `lib/scratchbird/connection.rb`
   - Change:
     - `Connection#close` now sets `@closed = true` in `ensure`.
   - Effect:
     - Close becomes idempotent and deterministic even if transport teardown raises.

3. New deterministic RES test suite
   - File: `test/test_resource_resilience.rb`
   - Added coverage:
     - connection close idempotence under injected disconnect failure
     - statement close idempotence when prepared deallocation raises
     - client close cleanup behavior with and without socket handle
     - `with_resilience` success/failure circuit-breaker and telemetry accounting
     - keepalive validation ping path
     - open-circuit rejection behavior

4. Cross-lane integration env fallback alignment
   - File: `test/test_integration.rb`
   - Change:
     - integration DSN now accepts `SCRATCHBIRD_RUBY_URL` or `SCRATCHBIRD_TEST_DSN`
     - cancel SQL now accepts `SCRATCHBIRD_RUBY_CANCEL_SQL` or `SCRATCHBIRD_TEST_CANCEL_SQL`
   - Effect:
     - reduces false skip rates when shared integration environments are already configured for other lanes.

## Targeted Tests Run

1. `ruby -Ilib:test test/test_resource_resilience.rb`
   - Result: PASS
   - Output summary: `8 runs, 47 assertions, 0 failures, 0 errors, 0 skips`

2. Full lane suite
   - Command: `ruby -Ilib:test -e 'Dir["test/test_*.rb"].sort.each { |f| system("ruby -Ilib:test #{f}") or exit(1) }'`
   - Result: PASS
   - Output summary (aggregate from file runs): `66 runs, 268 assertions, 0 failures, 0 errors, 8 skips`  
     (`test_integration.rb` remains env-gated and skipped without runtime DSN)

## RES Status Recommendation

- Recommendation: `IMPLEMENTED`
- Rationale:
  - Lane now has deterministic coverage for idempotent resource teardown and resilience helper behavior.
  - Runtime stream/result lifecycle behavior remains covered by existing result-stream tests.
  - Remaining gaps are live integration depth, not missing API/lifecycle surfaces.

## Remaining Gaps

1. Add live integration checks for keepalive/leak behavior under real network disruption and reconnect scenarios.
