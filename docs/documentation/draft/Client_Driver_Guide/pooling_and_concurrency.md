# Pooling and Concurrency

## Purpose

This page documents the `thread_safety_class` and `pooling_capability` values that appear
across the driver manifest, and explains what each value means for application developers
choosing a connection pooling strategy.

All values are verified from `project/drivers/DriverPackageManifest.csv`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Thread Safety Classes

The `thread_safety_class` column in the manifest describes whether a single driver connection
object is safe to use from multiple threads concurrently.

| Class | Manifest Value | Meaning for Application Code |
| --- | --- | --- |
| Thread-safe | `thread_safe` | The connection object and derived cursor/statement objects are safe for concurrent use from multiple threads. The driver manages internal synchronization. |
| Connection-thread-confined | `connection_thread_confined` | A connection object must be used from a single thread. Do not share a connection across threads. Create one connection per thread, or use a pool that enforces thread affinity when checking out connections. |

### Per-Driver Thread Safety

| Driver / Adaptor | Thread Safety Class |
| --- | --- |
| adbc | `thread_safe` |
| flightsql | `thread_safe` |
| julia | `thread_safe` |
| perl | `connection_thread_confined` |
| r2dbc | `thread_safe` |
| cpp | `thread_safe` |
| dart | `thread_safe` |
| dotnet | `thread_safe` |
| elixir | `thread_safe` |
| go | `thread_safe` |
| jdbc | `thread_safe` |
| mojo | `thread_safe` |
| node | `thread_safe` |
| odbc | `thread_safe` |
| pascal | `thread_safe` |
| php | `connection_thread_confined` |
| python | `thread_safe` |
| r | `connection_thread_confined` |
| ruby | `thread_safe` |
| rust | `thread_safe` |
| swift | `thread_safe` |
| scratchbird-airbyte | `connection_thread_confined` |
| scratchbird-dbt-adapter | `connection_thread_confined` |
| scratchbird-looker | `connection_thread_confined` |
| scratchbird-powerbi | `connection_thread_confined` |
| scratchbird-tableau | `connection_thread_confined` |
| scratchbird-dbeaver-driver | `connection_thread_confined` |
| scratchbird-hibernate-dialect | `connection_thread_confined` |
| scratchbird-metabase-driver | `connection_thread_confined` |
| scratchbird-prisma-adapter | `connection_thread_confined` |
| scratchbird-sqlalchemy-dialect | `connection_thread_confined` |
| scratchbird-superset-driver | `connection_thread_confined` |
| scratchbird-typeorm-adapter | `connection_thread_confined` |
| CLI tool | `connection_thread_confined` |

Source: `DriverPackageManifest.csv` column `thread_safety_class`.

**Pattern:** All application-layer adaptors are `connection_thread_confined`. This reflects
that adaptors delegate pooling to their embedded driver (Python, JDBC, or Node.js), and the
adaptor layer itself is not safe to share across threads.

---

## Pooling Capabilities

The `pooling_capability` column describes the type of connection pooling the driver supports.

| Capability | Manifest Value | Description |
| --- | --- | --- |
| Connection pool | `connection_pool` | The driver supports a pool of established connections that are checked out by callers and returned when done. The driver manages the pool lifecycle internally. |
| Session pool | `session_pool` | The driver supports a pool of established sessions (which may be lighter-weight than full connections). Callers check out a session. |
| Reactive pool | `reactive_pool` | The driver supports an asynchronous, reactive-streams-compatible connection pool. Used by the R2DBC driver in async/reactive application contexts. |
| Stream pool | `stream_pool` | The driver supports a pool for Arrow stream connections. Used by the FlightSQL driver. |
| Session pool + statement cache | `session_pool;statement_cache` | The driver supports session pooling combined with a per-session statement cache. Used by the C/C++ driver. |
| Explicit session | `explicit_session` | The driver opens and closes sessions explicitly; there is no automatic pool. Callers manage the session lifecycle directly. |
| Delegates to Python | `delegates_to_python` | The adaptor delegates pooling entirely to the embedded Python driver. The adaptor itself does not maintain a pool. |
| Delegates to JDBC | `delegates_to_jdbc` | The adaptor delegates pooling entirely to the embedded JDBC driver. |
| Delegates to Node | `delegates_to_node` | The adaptor delegates pooling entirely to the embedded Node.js driver. |

### Per-Driver Pooling Capability

| Driver / Adaptor | Pooling Capability |
| --- | --- |
| adbc | `connection_pool` |
| flightsql | `stream_pool` |
| julia | `connection_pool` |
| perl | `connection_pool` |
| r2dbc | `reactive_pool` |
| cpp | `session_pool;statement_cache` |
| dart | `session_pool` |
| dotnet | `connection_pool` |
| elixir | `session_pool` |
| go | `connection_pool` |
| jdbc | `connection_pool` |
| mojo | `session_pool` |
| node | `connection_pool` |
| odbc | `connection_pool` |
| pascal | `session_pool` |
| php | `session_pool` |
| python | `connection_pool` |
| r | `session_pool` |
| ruby | `connection_pool` |
| rust | `connection_pool` |
| swift | `session_pool` |
| scratchbird-airbyte | `delegates_to_python` |
| scratchbird-dbt-adapter | `delegates_to_python` |
| scratchbird-looker | `delegates_to_jdbc` |
| scratchbird-powerbi | `explicit_session` |
| scratchbird-tableau | `explicit_session` |
| scratchbird-dbeaver-driver | `delegates_to_jdbc` |
| scratchbird-hibernate-dialect | `delegates_to_jdbc` |
| scratchbird-metabase-driver | `delegates_to_jdbc` |
| scratchbird-prisma-adapter | `delegates_to_node` |
| scratchbird-sqlalchemy-dialect | `delegates_to_python` |
| scratchbird-superset-driver | `delegates_to_python` |
| scratchbird-typeorm-adapter | `delegates_to_node` |
| CLI tool | `explicit_session` |

Source: `DriverPackageManifest.csv` column `pooling_capability`.

---

## Guidance for Application Developers

**For thread_safe drivers with connection_pool or session_pool:**
You may share a pool object across threads. Individual connections or sessions from the pool
are scoped to the borrowing call and should not themselves be shared across threads while
checked out.

**For connection_thread_confined drivers:**
Create one connection per thread. If you need concurrent access, use a pool that enforces
thread affinity — i.e., a checked-out connection must be used and returned on the same thread
that checked it out.

**For delegates_to_* adaptors:**
The adaptor's pooling behavior is entirely determined by the embedded driver. Configure
pooling settings through that driver's native API (e.g., Python driver pool config, JDBC
connection pool config, Node.js driver pool config). The adaptor layer itself does not offer
pool configuration.

**For explicit_session:**
The application is responsible for opening, using, and closing sessions. This is appropriate
for tooling, administrative scripts, and BI tools that manage session lifecycle at the
application level.

---

## Pool Resource Lifecycle (Python driver, verified)

The Python driver pool implementation includes:
- Checkout / reuse / stale-connection replacement
- Statement cache per session
- Retry backoff
- Circuit-breaker state transitions
- Keepalive validation
- Leak-detection guard behavior
- Telemetry metrics and slow-query tracking

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — `RES` row, pool
and resilience primitives.

---

## Cross-References

- [conformance_baseline.md](conformance_baseline.md) — S1-S5 conformance stages including resource lifecycle (RES stage)
- [connection_and_dsn.md](connection_and_dsn.md) — how sessions are opened
