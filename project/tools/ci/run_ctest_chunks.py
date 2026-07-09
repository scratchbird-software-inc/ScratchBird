#!/usr/bin/env python3
"""Run a CTest preset in deterministic, non-overlapping chunks.

The script keeps a full public-release CTest run intact while giving CI logs and
artifacts smaller failure units.  It queries CTest's configured test inventory,
splits the numeric CTest order into contiguous chunks, runs each chunk, and then
returns a failing exit code if any chunk failed.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class Chunk:
    index: int
    start: int
    end: int
    tests: list[str]

    @property
    def name(self) -> str:
        return f"chunk_{self.index:02d}_{self.start:04d}_{self.end:04d}"


def run_capture(command: list[str], cwd: pathlib.Path) -> str:
    completed = subprocess.run(
        command,
        cwd=str(cwd),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        raise SystemExit(completed.returncode)
    return completed.stdout


def ctest_base_command(preset: str | None, test_dir: pathlib.Path | None) -> list[str]:
    if preset:
        return ["ctest", "--preset", preset]
    if test_dir:
        return ["ctest", "--test-dir", str(test_dir)]
    raise SystemExit("Either --preset or --test-dir is required")


def load_ctest_inventory(base_command: list[str], cwd: pathlib.Path) -> list[str]:
    raw = run_capture([*base_command, "-N", "--show-only=json-v1"], cwd)
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        sys.stderr.write(raw)
        raise SystemExit(f"CTest did not return json-v1 inventory: {exc}") from exc
    tests = [str(test["name"]) for test in payload.get("tests", [])]
    if not tests:
        raise SystemExit("CTest inventory is empty; refusing to report a false pass")
    return tests


def build_chunks(tests: list[str], chunk_count: int) -> list[Chunk]:
    chunk_count = max(1, min(chunk_count, len(tests)))
    width = int(math.ceil(len(tests) / chunk_count))
    chunks: list[Chunk] = []
    for index, start_zero in enumerate(range(0, len(tests), width), start=1):
        end_zero = min(start_zero + width, len(tests))
        chunks.append(
            Chunk(
                index=index,
                start=start_zero + 1,
                end=end_zero,
                tests=tests[start_zero:end_zero],
            )
        )
    return chunks


def write_json(path: pathlib.Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def stream_command(command: list[str], cwd: pathlib.Path, log_path: pathlib.Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + " ".join(command) + "\n\n")
        log.flush()
        process = subprocess.Popen(
            command,
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log.write(line)
        return process.wait()


def github_group(title: str) -> None:
    print(f"::group::{title}", flush=True)


def github_endgroup() -> None:
    print("::endgroup::", flush=True)


def make_manifest(chunks: Iterable[Chunk]) -> list[dict[str, object]]:
    return [
        {
            "chunk": chunk.name,
            "index": chunk.index,
            "start": chunk.start,
            "end": chunk.end,
            "test_count": len(chunk.tests),
            "tests": chunk.tests,
        }
        for chunk in chunks
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", help="CTest preset to run")
    parser.add_argument("--test-dir", type=pathlib.Path, help="Configured CTest build directory")
    parser.add_argument("--chunk-count", type=int, default=8, help="Target number of CTest chunks")
    parser.add_argument("--timeout", type=int, default=900, help="Per-test timeout passed to CTest")
    parser.add_argument(
        "--output-root",
        type=pathlib.Path,
        required=True,
        help="Directory for chunk manifests, summaries, and logs",
    )
    parser.add_argument(
        "--stop-on-failure",
        action="store_true",
        help="Stop after the first failing chunk instead of collecting all chunk failures",
    )
    parser.add_argument(
        "--chunk-index",
        type=int,
        help="Run only this one-based chunk index and write its result for later finalization",
    )
    parser.add_argument(
        "--finalize",
        action="store_true",
        help="Summarize previously recorded chunk results and fail if any chunk failed or is missing",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Write the manifest and summary without executing any tests",
    )
    args = parser.parse_args()

    cwd = pathlib.Path.cwd()
    output_root = args.output_root
    output_root.mkdir(parents=True, exist_ok=True)
    if bool(args.preset) == bool(args.test_dir):
        raise SystemExit("Pass exactly one of --preset or --test-dir")

    base_command = ctest_base_command(args.preset, args.test_dir)
    tests = load_ctest_inventory(base_command, cwd)
    chunks = build_chunks(tests, args.chunk_count)
    write_json(output_root / "ctest_chunk_manifest.json", make_manifest(chunks))
    if args.chunk_index is not None and (args.chunk_index < 1 or args.chunk_index > len(chunks)):
        raise SystemExit(f"--chunk-index must be between 1 and {len(chunks)}")

    if args.finalize:
        results: list[dict[str, object]] = []
        failures: list[dict[str, object]] = []
        missing: list[dict[str, object]] = []
        for chunk in chunks:
            path = output_root / "results" / f"{chunk.name}.json"
            if not path.is_file():
                item = {
                    "chunk": chunk.name,
                    "start": chunk.start,
                    "end": chunk.end,
                    "test_count": len(chunk.tests),
                    "reason": "missing_result",
                }
                missing.append(item)
                failures.append(item)
                continue
            result = json.loads(path.read_text(encoding="utf-8"))
            results.append(result)
            if int(result.get("returncode", 1)) != 0:
                failures.append(result)
        summary = {
            "preset": args.preset,
            "test_dir": str(args.test_dir) if args.test_dir else None,
            "chunk_count": len(chunks),
            "test_count": len(tests),
            "failed_chunk_count": len(failures),
            "missing_chunk_count": len(missing),
            "results": results,
            "missing": missing,
            "failures": failures,
        }
        write_json(output_root / "ctest_chunk_summary.json", summary)
        if failures:
            print(
                f"CTest chunk finalization failed: {len(failures)} failed/missing chunk(s). "
                f"See {output_root}."
            )
            return 1
        print(f"All {len(tests)} CTest tests passed across {len(chunks)} chunk(s).")
        return 0

    if args.dry_run:
        summary = {
            "preset": args.preset,
            "test_dir": str(args.test_dir) if args.test_dir else None,
            "chunk_count": len(chunks),
            "test_count": len(tests),
            "failed_chunk_count": 0,
            "dry_run": True,
            "results": [
                {
                    "chunk": chunk.name,
                    "start": chunk.start,
                    "end": chunk.end,
                    "test_count": len(chunk.tests),
                    "returncode": None,
                }
                for chunk in chunks
            ],
            "failures": [],
        }
        write_json(output_root / "ctest_chunk_summary.json", summary)
        print(f"Dry run: {len(tests)} CTest tests assigned across {len(chunks)} chunk(s).")
        return 0

    failures: list[dict[str, object]] = []
    results: list[dict[str, object]] = []
    selected_chunks = [chunks[args.chunk_index - 1]] if args.chunk_index is not None else chunks
    for chunk in selected_chunks:
        title = f"CTest {chunk.name}: tests {chunk.start}-{chunk.end} ({len(chunk.tests)} tests)"
        github_group(title)
        print(title)
        command = [
            *base_command,
            "-I",
            f"{chunk.start},{chunk.end}",
            "--progress",
            "--output-on-failure",
            "--timeout",
            str(args.timeout),
        ]
        log_path = output_root / "logs" / f"{chunk.name}.log"
        returncode = stream_command(command, cwd, log_path)
        result = {
            "chunk": chunk.name,
            "start": chunk.start,
            "end": chunk.end,
            "test_count": len(chunk.tests),
            "log": str(log_path),
            "returncode": returncode,
        }
        results.append(result)
        write_json(output_root / "results" / f"{chunk.name}.json", result)
        if returncode != 0:
            failures.append(result)
            print(f"CTest {chunk.name} failed with exit code {returncode}")
            if args.stop_on_failure:
                github_endgroup()
                break
        else:
            print(f"CTest {chunk.name} passed")
        github_endgroup()

    summary = {
        "preset": args.preset,
        "test_dir": str(args.test_dir) if args.test_dir else None,
        "chunk_count": len(chunks),
        "test_count": len(tests),
        "failed_chunk_count": len(failures),
        "results": results,
        "failures": failures,
    }
    write_json(output_root / "ctest_chunk_summary.json", summary)
    if failures:
        print(f"{len(failures)} CTest chunk(s) failed. See {output_root / 'logs'}.")
        return 1
    print(f"All {len(tests)} CTest tests passed across {len(chunks)} chunk(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
