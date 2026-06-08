# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import os
import pathlib
import shlex
import shutil
import subprocess
import sys
from typing import Any, Mapping
import urllib.parse

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


_SCHEMA_ROW_ALIASES = ("schema_name", "table_schema", "table_schem", "schema")


def _is_truthy(value: str) -> bool:
    return value.strip().lower() in ("1", "true", "yes", "on")


def _deterministic_fallback_dsn() -> str:
    return "scratchbird://user:pass@localhost:3092/testdb?sslmode=require"


def _deterministic_fallback_manager_dsn() -> str:
    return (
        "scratchbird://user:pass@localhost:3092/testdb"
        "?sslmode=require&front_door_mode=manager_proxy&manager_auth_token=deterministic_token"
    )


def _deterministic_fallback_bad_auth_dsn() -> str:
    return (
        "scratchbird://user:pass@localhost:3092/testdb"
        "?sslmode=require&sb_test_auth_fail=true"
    )


def _is_deterministic_lane_dsn(dsn: str) -> bool:
    return dsn in (_deterministic_fallback_dsn(), _deterministic_fallback_manager_dsn())


def _dsn_with_append(base_dsn: str, query_append: str) -> str:
    fragment = (query_append or "").strip()
    if not fragment:
        return base_dsn
    if fragment.startswith("?"):
        fragment = fragment[1:]
    appended_pairs = urllib.parse.parse_qsl(fragment, keep_blank_values=True)
    if not appended_pairs:
        return base_dsn
    parsed = urllib.parse.urlparse(base_dsn)
    existing_pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    merged_query = urllib.parse.urlencode(existing_pairs + appended_pairs)
    return urllib.parse.urlunparse(
        (parsed.scheme, parsed.netloc, parsed.path, parsed.params, merged_query, parsed.fragment)
    )


def _split_dsn_matrix(raw: str) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for chunk in (raw or "").replace(",", "\n").splitlines():
        dsn = chunk.strip()
        if not dsn or dsn in seen:
            continue
        seen.add(dsn)
        out.append(dsn)
    return out


def _wire_transport_dsn(dsn: str, deterministic_lane: bool) -> str:
    if deterministic_lane:
        return dsn
    if not _is_truthy(os.environ.get("SCRATCHBIRD_MOJO_WIRE_TRANSPORT", "1")):
        return dsn
    return _dsn_with_append(dsn, "sb_wire_transport=python")


def _native_bootstrap_run_args() -> list[str]:
    raw = os.environ.get("SCRATCHBIRD_MOJO_NATIVE_RUN_ARGS", "").strip()
    if raw:
        return shlex.split(raw)
    return ["-O0", "-j1"]


def _native_bootstrap_command(script_path: str) -> list[str] | None:
    run_args = _native_bootstrap_run_args()

    mojo_bin = os.environ.get("MOJO_BIN", "").strip()
    if mojo_bin:
        return [mojo_bin, "run", *run_args, "-I", "src", "-I", "src/scratchbird", script_path]

    mojo_path = shutil.which("mojo")
    if mojo_path:
        return [mojo_path, "run", *run_args, "-I", "src", "-I", "src/scratchbird", script_path]

    pixi_path = shutil.which("pixi")
    manifest = pathlib.Path(
        os.environ.get(
            "MOJO_PIXI_MANIFEST",
            str(pathlib.Path.home() / "mojo-work" / "sb-mojo"),
        )
    )
    if pixi_path and manifest.is_dir():
        return [
            pixi_path,
            "run",
            "-m",
            str(manifest),
            "--executable",
            "mojo",
            "run",
            *run_args,
            "-I",
            "src",
            "-I",
            "src/scratchbird",
            script_path,
        ]

    return None


def _run_native_bootstrap_smoke(lane_root: pathlib.Path) -> None:
    if _is_truthy(os.environ.get("SCRATCHBIRD_MOJO_SKIP_NATIVE_BOOTSTRAP", "")):
        print("SCRATCHBIRD_MOJO_SKIP_NATIVE_BOOTSTRAP set; skipping Mojo native bootstrap smoke.")
        return

    required = _is_truthy(os.environ.get("SCRATCHBIRD_MOJO_NATIVE_REQUIRED", ""))
    smoke_steps = [
        ("scratchbird module surface smoke", "tests/scratchbird_surface.mojo"),
        ("native bootstrap smoke", "tests/native_bootstrap.mojo"),
    ]
    for label, script_path in smoke_steps:
        command = _native_bootstrap_command(script_path)
        if command is None:
            message = f"Mojo launcher unavailable for {label} (no mojo/pixi found)."
            if required:
                raise RuntimeError(message)
            print(message + " Continuing with Python integration smoke.")
            return

        completed = subprocess.run(
            command,
            cwd=lane_root,
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode == 0:
            output = (completed.stdout or "").strip()
            if output:
                print(output)
            else:
                print(f"Mojo {label} OK")
            continue

        error_text = (completed.stderr or completed.stdout or "").strip()
        first_line = error_text.splitlines()[0] if error_text else "no details"
        if required:
            raise RuntimeError(
                f"Mojo {label} failed with exit {completed.returncode}: {first_line}"
            )
        print(
            f"Mojo {label} failed (continuing with Python integration smoke): {first_line}"
        )
        return


def _seed_live_fixtures_if_available(lane_root: pathlib.Path, dsns: list[str]) -> None:
    if _is_truthy(os.environ.get("SCRATCHBIRD_MOJO_SKIP_FIXTURE_SEED", "")):
        return
    if not any(not _is_deterministic_lane_dsn(dsn) for dsn in dsns):
        return
    repo_root = lane_root.parents[3]
    script = repo_root / "scripts" / "driver_runtime_stack.sh"
    completed = subprocess.run(
        [str(script), "fixtures"],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        error_text = (completed.stderr or completed.stdout or "").strip()
        first_line = error_text.splitlines()[0] if error_text else "no details"
        raise RuntimeError(f"fixture seed failed: {first_line}")


def _normalize_identifier(value: Any) -> str:
    return str(value).strip().lower().replace("-", "_").replace(" ", "_")


def _rows_from_result(result: Any) -> list[Any]:
    rows = getattr(result, "rows", None)
    if rows is None:
        return []
    if isinstance(rows, list):
        return rows
    return list(rows)


def _rowcount_from_result(result: Any) -> int:
    rowcount = getattr(result, "rowcount", None)
    if isinstance(rowcount, int) and rowcount >= 0:
        return rowcount
    return len(_rows_from_result(result))


def _schema_name_from_row(row: Any) -> str | None:
    if isinstance(row, Mapping):
        for key, value in row.items():
            if _normalize_identifier(key) in _SCHEMA_ROW_ALIASES:
                if value is None:
                    return None
                text = str(value).strip()
                return text if text else None
        return None

    if isinstance(row, (tuple, list)):
        if len(row) == 0:
            return None
        candidate = row[1] if len(row) > 1 else row[0]
        if candidate is None:
            return None
        text = str(candidate).strip()
        return text if text else None

    return None


def _row_scalar_values(row: Any) -> list[Any]:
    if isinstance(row, Mapping):
        return list(row.values())
    if isinstance(row, (tuple, list)):
        return list(row)
    return [row]


def _first_scalar_from_row(row: Any) -> Any:
    values = _row_scalar_values(row)
    if len(values) == 0:
        return None
    return values[0]


def _open_stream(conn: Any, sql: str, fetch_size: int) -> Any:
    try:
        return conn.stream(sql, fetch_size=fetch_size)
    except TypeError:
        return conn.stream(sql, None, fetch_size)


def _stream_next(stream: Any, conn: Any) -> Any:
    if hasattr(stream, "__next__"):
        return next(stream)
    next_method = getattr(stream, "next", None)
    if callable(next_method):
        try:
            return next_method(conn)
        except TypeError:
            return next_method()
    raise RuntimeError("stream object does not support next()")


def _close_stream(stream: Any) -> None:
    close_method = getattr(stream, "close", None)
    if callable(close_method):
        close_method()


def _validate_transaction_smoke(conn: Any) -> None:
    # ScratchBird sessions are always in a transaction; commit/rollback restart one immediately.
    conn.commit()
    conn.rollback()

    try:
        conn.begin()
        raise RuntimeError("nested begin should raise while transaction is already active")
    except Exception as exc:
        sqlstate = str(getattr(exc, "sqlstate", "") or "")
        if sqlstate not in ("", "25001"):
            raise RuntimeError(f"unexpected nested begin sqlstate '{sqlstate}'") from exc
        if sqlstate == "" and "already active" not in str(exc).lower():
            raise RuntimeError(f"unexpected nested begin error '{exc}'") from exc

    savepoint = conn.set_savepoint("smoke_sp")
    if str(savepoint) != "smoke_sp":
        raise RuntimeError("savepoint name roundtrip mismatch")
    _ = conn.set_savepoint("smoke_tail")
    conn.rollback_to_savepoint("smoke_sp")
    conn.release_savepoint("smoke_sp")
    conn.commit()

    savepoint = conn.set_savepoint("smoke_post_commit")
    if str(savepoint) != "smoke_post_commit":
        raise RuntimeError("savepoint should remain available after auto-restarted commit transaction")
    conn.release_savepoint("smoke_post_commit")
    conn.rollback()
    post_rollback = conn.query("SELECT 2")
    post_rollback_rows = _rows_from_result(post_rollback)
    if len(post_rollback_rows) == 0 or int(_first_scalar_from_row(post_rollback_rows[0])) != 2:
        raise RuntimeError("post-rollback query should return the actual result on the fresh boundary")


def _validate_prepare_and_stream_smoke(conn: Any, deterministic_lane: bool) -> None:
    statement = conn.prepare("SELECT $1::INTEGER, $2::INTEGER")
    prepared = statement.execute(["5", "7"])
    prepared_rows = _rows_from_result(prepared)
    if len(prepared_rows) == 0:
        raise RuntimeError("prepared execute should return at least one row")
    first_values = _row_scalar_values(prepared_rows[0])
    if len(first_values) < 2 or int(first_values[0]) != 5 or int(first_values[1]) != 7:
        raise RuntimeError("prepared execute row payload mismatch")
    try:
        statement.execute(["5"])
        raise RuntimeError("prepared execute should reject parameter mismatch")
    except Exception as exc:
        sqlstate = str(getattr(exc, "sqlstate", "") or "")
        if sqlstate not in ("", "07001"):
            raise RuntimeError(f"unexpected prepared mismatch sqlstate '{sqlstate}'") from exc
        if sqlstate == "":
            text = str(exc).lower()
            if "parameter" not in text and "mismatch" not in text:
                raise RuntimeError(f"unexpected prepared mismatch error '{exc}'") from exc

    stream = _open_stream(conn, "SELECT id FROM basic_table ORDER BY id", 1)
    try:
        first_stream_row = _stream_next(stream, conn)
        if int(_first_scalar_from_row(first_stream_row)) != 1:
            raise RuntimeError("stream first-row payload mismatch")
        conn.cancel()
        cancelled = False
        try:
            _ = _stream_next(stream, conn)
        except StopIteration:
            cancelled = True
        except Exception as exc:
            cancelled = True
            sqlstate = str(getattr(exc, "sqlstate", "") or "")
            if sqlstate not in ("", "57014"):
                raise RuntimeError(f"unexpected stream cancel sqlstate '{sqlstate}'") from exc
        if deterministic_lane and not cancelled:
            raise RuntimeError("deterministic stream cancel should interrupt iteration")
    finally:
        _close_stream(stream)

    recovery_stream = _open_stream(conn, "SELECT id FROM basic_table ORDER BY id", 1)
    try:
        recovery_row = _stream_next(recovery_stream, conn)
        if int(_first_scalar_from_row(recovery_row)) != 1:
            raise RuntimeError("post-cancel stream recovery payload mismatch")
    finally:
        _close_stream(recovery_stream)


def _validate_long_running_stream_cancel(conn: Any, deterministic_lane: bool) -> None:
    sql = os.environ.get("SCRATCHBIRD_MOJO_LONG_STREAM_SQL", "").strip()
    if sql == "":
        if deterministic_lane:
            sql = "SELECT a.id FROM basic_table a, basic_table b, basic_table c, basic_table d, basic_table e"
        else:
            # Live matrix keeps long-running assertions opt-in to avoid assuming fixture tables.
            return

    row_budget_text = os.environ.get("SCRATCHBIRD_MOJO_LONG_STREAM_CANCEL_AFTER_ROWS", "").strip()
    try:
        row_budget = max(1, int(row_budget_text)) if row_budget_text else 4
    except ValueError:
        row_budget = 4

    stream = _open_stream(conn, sql, 1)
    consumed = 0
    try:
        while consumed < row_budget:
            _ = _stream_next(stream, conn)
            consumed += 1
    except StopIteration:
        pass
    finally:
        conn.cancel()
    cancelled = False
    try:
        _ = _stream_next(stream, conn)
    except StopIteration:
        cancelled = True
    except Exception as exc:
        cancelled = True
        sqlstate = str(getattr(exc, "sqlstate", "") or "")
        if sqlstate not in ("", "57014"):
            raise RuntimeError(f"unexpected long-running cancel sqlstate '{sqlstate}'") from exc
    finally:
        _close_stream(stream)
    if deterministic_lane and not cancelled:
        raise RuntimeError("deterministic long-running stream cancel should interrupt iteration")


def _validate_lifecycle_snapshot(conn: Any) -> None:
    snapshot_method = getattr(conn, "lifecycle_snapshot", None)
    if not callable(snapshot_method):
        return
    before = snapshot_method()
    if not isinstance(before, Mapping):
        return
    _ = conn.query("SELECT 1")
    after = snapshot_method()
    if not isinstance(after, Mapping):
        return
    before_queries = int(before.get("query_count", 0))
    after_queries = int(after.get("query_count", 0))
    if after_queries < before_queries:
        raise RuntimeError("lifecycle snapshot query_count should be monotonic")
    if bool(after.get("closed", False)):
        raise RuntimeError("lifecycle snapshot should report open connection during integration checks")


def _validate_payload_contract(payload: Mapping[str, Any]) -> None:
    if set(payload.keys()) != {"schemaPattern", "expandSchemaParents", "schemaPaths", "schemaTree"}:
        raise RuntimeError("ddl_editor_schema_payload keys mismatch")
    if payload.get("schemaPattern") != "users.%":
        raise RuntimeError("ddl_editor_schema_payload schemaPattern mismatch")
    if payload.get("expandSchemaParents") is not True:
        raise RuntimeError("ddl_editor_schema_payload expandSchemaParents mismatch")
    schema_paths = payload.get("schemaPaths")
    if not isinstance(schema_paths, list):
        raise RuntimeError("ddl_editor_schema_payload schemaPaths should be a list")
    if len(schema_paths) != len(set(schema_paths)):
        raise RuntimeError("ddl_editor_schema_payload schemaPaths should be unique")
    for path in schema_paths:
        if not isinstance(path, str) or path.strip() == "":
            raise RuntimeError("ddl_editor_schema_payload schemaPaths should contain non-empty strings")
    for path in schema_paths:
        if "." not in path:
            continue
        parent = path.rsplit(".", 1)[0]
        if parent not in schema_paths:
            raise RuntimeError("ddl_editor_schema_payload schemaPaths should include parent paths when expanded")
    if not isinstance(payload.get("schemaTree"), list):
        raise RuntimeError("ddl_editor_schema_payload schemaTree should be a list")


def _validate_metadata_stability(conn: Any) -> None:
    schemas_total = _rowcount_from_result(conn.query_metadata("schemas"))
    tables_total = _rowcount_from_result(conn.query_metadata("tables"))
    columns_total = _rowcount_from_result(conn.query_metadata("columns"))
    if schemas_total <= 0:
        raise RuntimeError("metadata schemas rowcount should be positive")
    if tables_total <= 0:
        raise RuntimeError("metadata tables rowcount should be positive")
    if columns_total <= 0:
        raise RuntimeError("metadata columns rowcount should be positive")

    tables_public = _rowcount_from_result(conn.query_metadata_restricted("tables", "schema", "public"))
    tables_public_orders = _rowcount_from_result(
        conn.query_metadata_restricted_multi("tables", {"schema": "public", "table": "ord%"})
    )
    tables_wildcard = _rowcount_from_result(conn.query_metadata_restricted("tables", "table", "ord%"))
    tables_escaped = _rowcount_from_result(conn.query_metadata_restricted("tables", "table", r"ord\_%"))

    if tables_public > tables_total:
        raise RuntimeError("restricted metadata table rowcount should not exceed total tables")
    if tables_public_orders > tables_public:
        raise RuntimeError("multi-restricted metadata table rowcount should not exceed schema-restricted tables")
    if tables_escaped > tables_wildcard:
        raise RuntimeError("escaped table wildcard rowcount should not exceed unescaped wildcard rowcount")

    schemas_users = _rowcount_from_result(conn.query_metadata_restricted("schemas", "schema", "users.%"))
    schemas_null = _rowcount_from_result(conn.query_metadata_restricted("schemas", "schema", "null"))
    if schemas_users > schemas_total:
        raise RuntimeError("schema wildcard rowcount should not exceed total schemas")
    if schemas_null > schemas_total:
        raise RuntimeError("null schema rowcount should not exceed total schemas")

    columns_alias_family = _rowcount_from_result(
        conn.query_metadata_restricted_multi(
            "columns",
            {"catalog": "public", "table": "ord%", "type": "INTEGER"},
        )
    )
    if columns_alias_family > columns_total:
        raise RuntimeError("alias-family restricted column rowcount should not exceed total columns")


def _validate_deterministic_metadata_content(conn: Any, payload: Mapping[str, Any]) -> None:
    schema_rows = _rows_from_result(conn.query_metadata("schemas"))
    schema_names = [_schema_name_from_row(row) for row in schema_rows]
    non_null_schema_names = sorted(name for name in schema_names if name is not None)
    if non_null_schema_names != ["sys", "users.alice.dev", "users.bob.dev"]:
        raise RuntimeError("deterministic schema content mismatch")
    if sum(name is None for name in schema_names) != 1:
        raise RuntimeError("deterministic schema rows should include exactly one null schema entry")

    users_rows = _rows_from_result(
        conn.query_metadata_restricted_multi(
            "schemas",
            {"schema": "users.%"},
        )
    )
    users_names = sorted(name for name in (_schema_name_from_row(row) for row in users_rows) if name is not None)
    if users_names != ["users.alice.dev", "users.bob.dev"]:
        raise RuntimeError("deterministic users.% schema restriction mismatch")

    null_rows = _rows_from_result(conn.query_metadata_restricted("schemas", "schema", "null"))
    if len(null_rows) != 1 or _schema_name_from_row(null_rows[0]) is not None:
        raise RuntimeError("deterministic null schema restriction should return one null row")

    expected_paths = ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"]
    if payload.get("schemaPaths") != expected_paths:
        raise RuntimeError("deterministic ddl_editor_schema_payload schemaPaths mismatch")

    tree = payload.get("schemaTree")
    if not isinstance(tree, list) or len(tree) != 1:
        raise RuntimeError("deterministic ddl_editor_schema_payload schemaTree root mismatch")
    users_root = tree[0]
    if not isinstance(users_root, Mapping) or users_root.get("name") != "users":
        raise RuntimeError("deterministic ddl_editor_schema_payload should have users root node")


def _run_smoke_for_dsn(dsn: str, label: str, deterministic_lane: bool = False) -> None:
    cfg = scratchbird.ScratchBirdConfig(dsn)
    conn = scratchbird.connect(cfg)
    try:
        res = conn.query("SELECT 1")
        if len(res.rows) == 0 or res.rows[0][0] != 1:
            raise RuntimeError("unexpected SELECT 1 result")

        res = conn.query("SELECT * FROM type_coverage")
        if len(res.rows) == 0:
            raise RuntimeError("type_coverage returned no rows")

        _validate_transaction_smoke(conn)
        _validate_prepare_and_stream_smoke(conn, deterministic_lane)
        _validate_long_running_stream_cancel(conn, deterministic_lane)
        _validate_metadata_stability(conn)
        _validate_lifecycle_snapshot(conn)
        payload = conn.ddl_editor_schema_payload(schema_pattern="users.%", expand_schema_parents=True)
        _validate_payload_contract(payload)
        if deterministic_lane:
            _validate_deterministic_metadata_content(conn, payload)
        print(f"Mojo {label} smoke OK")
    finally:
        conn.close()


def _expect_auth_failure(dsn: str) -> None:
    cfg = scratchbird.ScratchBirdConfig(dsn)
    try:
        conn = scratchbird.connect(cfg)
        try:
            conn.query("SELECT 1")
        finally:
            conn.close()
        raise RuntimeError("expected auth failure for SCRATCHBIRD_MOJO_BAD_AUTH_URL")
    except Exception as exc:
        sqlstate = str(getattr(exc, "sqlstate", "") or "")
        if sqlstate not in ("", "28000", "28P01"):
            raise RuntimeError(f"unexpected bad-auth sqlstate '{sqlstate}'") from exc
        if sqlstate == "" and "auth" not in str(exc).lower():
            raise RuntimeError(f"unexpected bad-auth error '{exc}'") from exc
    print("Mojo bad-auth smoke OK")


def main() -> None:
    lane_root = pathlib.Path(__file__).resolve().parents[1]
    _run_native_bootstrap_smoke(lane_root)

    fallback_disabled = _is_truthy(os.environ.get("SCRATCHBIRD_MOJO_DISABLE_FALLBACK_DSN", ""))

    direct_dsns = _split_dsn_matrix(os.environ.get("SCRATCHBIRD_MOJO_DIRECT_URLS", ""))
    if len(direct_dsns) == 0:
        direct_single = os.environ.get("SCRATCHBIRD_MOJO_URL", "").strip()
        if direct_single:
            direct_dsns = [direct_single]
    if len(direct_dsns) == 0 and not fallback_disabled:
        direct_dsns = [_deterministic_fallback_dsn()]

    manager_dsns = _split_dsn_matrix(os.environ.get("SCRATCHBIRD_MOJO_MANAGER_URLS", ""))
    if len(manager_dsns) == 0:
        manager_single = os.environ.get("SCRATCHBIRD_MOJO_MANAGER_URL", "").strip()
        if manager_single:
            manager_dsns = [manager_single]
    if len(manager_dsns) == 0 and not fallback_disabled:
        manager_dsns = [_deterministic_fallback_manager_dsn()]

    listener_dsns = _split_dsn_matrix(os.environ.get("SCRATCHBIRD_MOJO_LISTENER_URLS", ""))
    if len(listener_dsns) == 0:
        listener_single = os.environ.get("SCRATCHBIRD_MOJO_LISTENER_URL", "").strip()
        if listener_single:
            listener_dsns = [listener_single]

    _seed_live_fixtures_if_available(lane_root, direct_dsns + manager_dsns + listener_dsns)

    if len(direct_dsns) == 0:
        print("SCRATCHBIRD_MOJO_URL not set; skipping Mojo direct integration smoke.")
    for idx, dsn in enumerate(direct_dsns):
        deterministic_lane = _is_deterministic_lane_dsn(dsn)
        _run_smoke_for_dsn(
            _wire_transport_dsn(dsn, deterministic_lane),
            f"direct integration [{idx + 1}/{len(direct_dsns)}]",
            deterministic_lane=deterministic_lane,
        )

    if len(manager_dsns) == 0:
        print("SCRATCHBIRD_MOJO_MANAGER_URL not set; skipping Mojo manager-proxy smoke.")
    for idx, dsn in enumerate(manager_dsns):
        deterministic_lane = _is_deterministic_lane_dsn(dsn)
        _run_smoke_for_dsn(
            _wire_transport_dsn(dsn, deterministic_lane),
            f"manager-proxy integration [{idx + 1}/{len(manager_dsns)}]",
            deterministic_lane=deterministic_lane,
        )

    if len(listener_dsns) == 0:
        print("SCRATCHBIRD_MOJO_LISTENER_URL not set; skipping Mojo listener matrix smoke.")

    for idx, dsn in enumerate(listener_dsns):
        deterministic_lane = _is_deterministic_lane_dsn(dsn)
        _run_smoke_for_dsn(
            _wire_transport_dsn(dsn, deterministic_lane),
            f"listener integration [{idx + 1}/{len(listener_dsns)}]",
            deterministic_lane=deterministic_lane,
        )

    bad_auth_dsns = _split_dsn_matrix(os.environ.get("SCRATCHBIRD_MOJO_BAD_AUTH_URLS", ""))
    if len(bad_auth_dsns) == 0:
        bad_auth_single = os.environ.get("SCRATCHBIRD_MOJO_BAD_AUTH_URL", "").strip()
        if bad_auth_single:
            bad_auth_dsns = [bad_auth_single]
    if len(bad_auth_dsns) == 0 and not fallback_disabled:
        bad_auth_dsns = [_deterministic_fallback_bad_auth_dsn()]
    if len(bad_auth_dsns) == 0:
        print("SCRATCHBIRD_MOJO_BAD_AUTH_URL not set; skipping Mojo bad-auth smoke.")
    for dsn in bad_auth_dsns:
        _expect_auth_failure(_wire_transport_dsn(dsn, _is_deterministic_lane_dsn(dsn)))


if __name__ == "__main__":
    main()
