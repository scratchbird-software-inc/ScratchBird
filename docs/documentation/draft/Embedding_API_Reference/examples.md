# Examples

## Purpose

This chapter walks through the real programs and scripts in `project/examples/`. There are two example packs: `public_engine_consumer_smoke` (a standalone C program that links against the public embedding API) and `core_beta_qa` (three shell scripts that drive built CTest fixtures). A third directory, `public_smoke_suite`, contains a JSON manifest describing staged CTest operations but no directly runnable scripts.

All examples share the invariant stated in the `core_beta_qa` manifest: engine execution remains SBLR/internal API based; SQL text is parser/client input only and is not runtime authority.

## Pack 1: `public_engine_consumer_smoke`

**Files:**
- `project/examples/public_engine_consumer_smoke/main.c`
- `project/examples/public_engine_consumer_smoke/CMakeLists.txt`

### CMake Entry Point

```cmake
find_package(ScratchBirdEngine CONFIG REQUIRED)

add_executable(scratchbird_public_engine_consumer_smoke main.c)
target_link_libraries(scratchbird_public_engine_consumer_smoke
    PRIVATE ScratchBird::sb_engine)
```

This is the canonical pattern for embedding: `find_package(ScratchBirdEngine CONFIG REQUIRED)` and link `ScratchBird::sb_engine`. The program is pure C and includes only `scratchbird/engine/engine.h`.

### `main.c` Walk-Through

The program demonstrates the full open → session → dispatch → result → close sequence entirely through the C ABI.

**Step 1: ABI version check.**

```c
if (sb_engine_abi_version_packed() != SB_ENGINE_ABI_VERSION_PACKED) {
    return 1;
}
```

The first thing any embedder should do is verify the runtime library version matches the compile-time header version. A mismatch returns 1 and the program does not proceed.

**Step 2: Build ID retrieval.**

```c
const char* build_id = 0;
uint64_t build_id_size = 0;
if (sb_engine_abi_build_id(&build_id, &build_id_size) != SB_ENGINE_STATUS_OK ||
    build_id == 0 || build_id_size == 0) {
    return 2;
}
```

Confirms the build ID is populated and non-empty. The content is opaque; the program only checks it is present.

**Step 3: Open engine in validation-only mode.**

```c
sb_engine_open_params_v1_t open_params;
memset(&open_params, 0, sizeof(open_params));
open_params.struct_size = sizeof(open_params);
open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;

sb_engine_handle_t engine = 0;
sb_engine_result_t result = 0;
if (sb_engine_open(&open_params, &engine, &result) != SB_ENGINE_STATUS_OK ||
    engine == 0) {
    return 3;
}
```

Note the `memset` to zero before populating — this ensures reserved fields are zero. `SB_ENGINE_OPEN_VALIDATION_ONLY` means no database path is required and no persistent writes occur. This is the correct mode for a smoke test.

**Step 4: Begin a session.**

```c
sb_engine_session_params_v1_t session_params;
memset(&session_params, 0, sizeof(session_params));
session_params.struct_size = sizeof(session_params);
session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
session_params.effective_user_uuid = uuid_with_tail(1);
session_params.session_uuid = uuid_with_tail(2);
session_params.default_language_utf8 = "en";
session_params.default_language_size = 2;
session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;

sb_engine_session_t session = 0;
if (sb_engine_session_begin(engine, &session_params, &session, &result) !=
        SB_ENGINE_STATUS_OK || session == 0) {
    (void)sb_engine_close(engine, 0);
    return 4;
}
```

The helper `uuid_with_tail(uint8_t tail)` constructs an `sb_engine_uuid_t` with `bytes[0] = 0x01`, `bytes[6] = 0x70`, and `bytes[15] = tail` (all others zero). This illustrates that UUIDs are application-constructed byte arrays; the engine does not generate them at session-begin time.

**Step 5: Construct a request context and dispatch (capability probe).**

```c
sb_engine_request_context_v1_t context;
memset(&context, 0, sizeof(context));
context.struct_size = sizeof(context);
context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
context.effective_user_uuid = session_params.effective_user_uuid;
context.session_uuid = session_params.session_uuid;
context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
context.rights_set_ref = 1;
context.capability_set_ref = 1;

sb_engine_sblr_dispatch_params_v1_t dispatch;
memset(&dispatch, 0, sizeof(dispatch));
dispatch.struct_size = sizeof(dispatch);
dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
/* envelope_bytes is NULL, envelope_size_bytes is 0 */

if (sb_engine_dispatch_sblr(session, 0, &context, &dispatch, &result) !=
        SB_ENGINE_STATUS_OK || result == 0) {
    (void)sb_engine_session_end(session, 0, 0);
    (void)sb_engine_close(engine, 0);
    return 5;
}
```

With `envelope_bytes = NULL` and `envelope_size_bytes = 0`, the dispatch acts as a capability probe. The transaction argument is `0` (NULL) since no transaction is needed. The `rights_set_ref = 1` and `capability_set_ref = 1` are minimal opaque references for the validation-only mode.

**Step 6: Check result class.**

```c
sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
    result_class != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
    return 6;
}
```

A successful capability probe returns `SB_ENGINE_RESULT_CAPABILITY_REPORT`.

**Step 7: Read and verify the payload.**

```c
sb_engine_string_view_t payload;
memset(&payload, 0, sizeof(payload));
if (sb_engine_result_payload(result, &payload) != SB_ENGINE_STATUS_OK ||
    payload.data == 0 || payload.size_bytes == 0 ||
    !contains_bytes(payload.data, payload.size_bytes, "capability probe", 16)) {
    return 7;
}
```

The smoke test confirms the payload contains the substring `"capability probe"`. The payload encoding for capability reports is not specified in the public headers; the program checks for a known substring rather than parsing a structured format.

**Step 8: Release result, end session, close engine.**

```c
if (sb_engine_result_release(result) != SB_ENGINE_STATUS_OK) {
    return 8;
}

sb_engine_session_end_params_v1_t end_params;
memset(&end_params, 0, sizeof(end_params));
end_params.struct_size = sizeof(end_params);
end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
end_params.rollback_active_transactions = 1;
end_params.cancel_open_results = 1;
if (sb_engine_session_end(session, &end_params, 0) != SB_ENGINE_STATUS_OK) {
    return 9;
}
if (sb_engine_close(engine, 0) != SB_ENGINE_STATUS_OK) {
    return 10;
}
return 0;
```

Teardown always proceeds in reverse acquisition order: result, session, engine. The `rollback_active_transactions = 1` and `cancel_open_results = 1` session-end parameters are the recommended safe teardown pattern.

## Pack 2: `core_beta_qa`

**Files:**
- `project/examples/core_beta_qa/admin_lifecycle_smoke.sh`
- `project/examples/core_beta_qa/embedded_public_abi_smoke.sh`
- `project/examples/core_beta_qa/driver_route_smoke.sh`
- `project/examples/core_beta_qa/manifest.json`
- `project/examples/core_beta_qa/README.md`

These scripts require a built tree. They accept two positional arguments: `REPO_ROOT` (default: the repository root inferred from the script's own location) and `BUILD_ROOT` (default: `$REPO_ROOT/build`).

Run from the repository root:

```bash
project/examples/core_beta_qa/admin_lifecycle_smoke.sh    "$PWD" "$PWD/build"
project/examples/core_beta_qa/embedded_public_abi_smoke.sh "$PWD" "$PWD/build"
project/examples/core_beta_qa/driver_route_smoke.sh        "$PWD" "$PWD/build"
```

All scripts use `set -euo pipefail`, create a temporary work directory under `$TMPDIR` or `/tmp`, and clean up on exit.

### `admin_lifecycle_smoke.sh`

Runs the built `database_lifecycle_admin_cli_conformance` fixture:

```bash
FIXTURE="${BUILD_ROOT}/tests/database_lifecycle/database_lifecycle_admin_cli_conformance"
"${FIXTURE}"
echo "admin_lifecycle_smoke=passed"
```

This fixture covers the database create/open/attach/detach lifecycle through the admin CLI conformance gate. The script comment is explicit: it does not ask the engine to execute SQL text; parser/client text remains outside the engine authority boundary.

### `embedded_public_abi_smoke.sh`

Runs two built fixtures back to back:

```bash
ABI_FIXTURE="${BUILD_ROOT}/tests/engine_public_abi/sb_engine_public_abi_cpp_fixture"
SBLR_FIXTURE="${BUILD_ROOT}/tests/engine_public_abi/sb_engine_public_sblr_admission_fixture"
"${ABI_FIXTURE}"
"${SBLR_FIXTURE}"
echo "embedded_public_abi_smoke=passed"
```

- `sb_engine_public_abi_cpp_fixture` exercises the public C++ ABI directly (the C++ RAII wrappers and C ABI functions).
- `sb_engine_public_sblr_admission_fixture` exercises SBLR admission — constructing and dispatching SBLR envelopes through the public dispatch path, verifying that the Priority D admission gate behaves correctly.

The `core_beta_qa/manifest.json` records the engine execution boundary: "Engine execution remains SBLR/internal API based; SQL text is parser/client input only and is not runtime authority."

### `driver_route_smoke.sh`

Runs representative driver gates through CTest:

```bash
ctest --test-dir "${BUILD_ROOT}" \
  -R "driver_package_manifest_gate|driver_python_gate|driver_cpp_gate" \
  --output-on-failure
echo "driver_route_smoke=passed"
```

This does not exercise the embedding API directly; it validates the driver package manifest gate and representative Python and C++ driver gates. Optional driver toolchains have their own deterministic skip/waiver behavior inside those CTest targets.

## Pack 3: `public_smoke_suite` (Manifest Only)

**File:**
- `project/examples/public_smoke_suite/manifest.json`

This pack contains only a JSON manifest (no directly runnable scripts). The manifest describes a sequence of staged CTest operations (create, open, schema, insert, select, transaction, rollback, backup, verify, diagnostics) that reference built test fixtures by CTest target name. It serves as the operations inventory for staged public smoke testing.

Key policy field from the manifest:

```json
{
  "sql_text_runtime_authority": false,
  "engine_execution_authority": "sblr_internal_api_uuid_mga"
}
```

The `public_smoke_suite` manifest refers to fixtures that are not co-located in the examples directory; they are in `project/tools/release/` and `project/tests/release/`. This pack is intended for use by the build/release toolchain rather than as standalone runnable scripts.

## Summary of Example Coverage

| Example | Language | What It Tests |
| --- | --- | --- |
| `public_engine_consumer_smoke/main.c` | C | Full open/session/dispatch/result/close cycle against the C ABI |
| `core_beta_qa/embedded_public_abi_smoke.sh` | Shell | C++ ABI and SBLR admission fixtures |
| `core_beta_qa/admin_lifecycle_smoke.sh` | Shell | Database lifecycle admin CLI conformance |
| `core_beta_qa/driver_route_smoke.sh` | Shell | Driver package manifest and representative driver gates |
| `public_smoke_suite/manifest.json` | JSON | Staged CTest operation inventory (not directly runnable) |
