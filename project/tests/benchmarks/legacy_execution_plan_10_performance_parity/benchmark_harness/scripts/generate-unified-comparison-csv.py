#!/usr/bin/env python3
"""
Generate a single cross-engine comparison CSV for a benchmark matrix run.
"""

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from benchmark_provenance import (
    PROVENANCE_FILENAME,
    provenance_file_for_result,
    validate_scratchbird_result_provenance,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create one unified CSV from a matrix run so each metric can be "
            "compared side-by-side across engines."
        )
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=None,
        help="Matrix output root directory (contains matrix-summary.json).",
    )
    parser.add_argument(
        "--summary",
        type=Path,
        default=None,
        help="Path to matrix-summary.json. Overrides --output-root lookup.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="CSV output path (default: <output-root>/matrix-comparison-unified.csv).",
    )
    return parser.parse_args()


def load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def unique_in_order(values: Iterable[str]) -> List[str]:
    seen = set()
    ordered = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        ordered.append(value)
    return ordered


def flatten_scalars(prefix: str, value: Any, out: Dict[str, Any]) -> None:
    if isinstance(value, dict):
        for child_key, child_value in value.items():
            child_prefix = f"{prefix}.{child_key}" if prefix else str(child_key)
            flatten_scalars(child_prefix, child_value, out)
        return

    if isinstance(value, list):
        key = f"{prefix}.count" if prefix else "count"
        out[key] = len(value)
        return

    out[prefix] = value


def count_nested_lists(value: Any) -> int:
    if not isinstance(value, dict):
        return 0
    total = 0
    for child in value.values():
        if isinstance(child, list):
            total += len(child)
    return total


def pick_suite_json_file(output_root: Path, engine: str, suite: str) -> Path | None:
    suite_dir = output_root / engine / suite
    if not suite_dir.exists():
        return None

    candidates = sorted(suite_dir.glob("*.json"))
    if not candidates:
        return None

    # Regression emits a summary file as the main artifact.
    if suite == "regression":
        for candidate in candidates:
            if "summary" in candidate.name:
                return candidate
        return candidates[0]

    ignored_names = {
        PROVENANCE_FILENAME,
        "comparison-guard.json",
        "system-info.json",
    }
    preferred = [
        item
        for item in candidates
        if "summary" not in item.name
        and item.name not in ignored_names
        and "lane" not in item.name
    ]
    if preferred:
        return preferred[0]

    non_summary = [item for item in candidates if "summary" not in item.name]
    if non_summary:
        return non_summary[0]
    return candidates[0]


def extract_suite_metrics(suite: str, payload: Dict[str, Any]) -> Dict[str, Any]:
    metrics: Dict[str, Any] = {}

    if suite == "regression":
        totals = payload.get("totals")
        if isinstance(totals, dict):
            flatten_scalars("totals", totals, metrics)
    else:
        summary = payload.get("summary")
        if isinstance(summary, dict):
            flatten_scalars("summary", summary, metrics)

    results = payload.get("results")
    if isinstance(results, list):
        metrics["results.count"] = len(results)
    elif isinstance(results, dict):
        nested_list_count = count_nested_lists(results)
        metrics["results.count"] = nested_list_count if nested_list_count else len(results)
        for key, value in results.items():
            if isinstance(value, list):
                metrics[f"results.{key}.count"] = len(value)

    test_results = payload.get("test_results")
    if isinstance(test_results, list):
        metrics["test_results.count"] = len(test_results)

    return metrics


def as_csv_value(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def suite_sort_key(suite: str, ordered_suites: List[str]) -> Tuple[int, str]:
    try:
        return (ordered_suites.index(suite), suite)
    except ValueError:
        return (len(ordered_suites), suite)


def main() -> int:
    args = parse_args()

    summary_path = args.summary
    if summary_path is None:
        if args.output_root is None:
            print("error: provide --summary or --output-root", file=sys.stderr)
            return 2
        summary_path = args.output_root / "matrix-summary.json"

    if not summary_path.exists():
        print(f"error: summary file not found: {summary_path}", file=sys.stderr)
        return 2

    summary = load_json(summary_path)
    output_root = args.output_root or Path(summary.get("output_root", ""))
    if not output_root:
        output_root = summary_path.parent

    engines = unique_in_order(summary.get("engines_requested", []))
    suites = unique_in_order(summary.get("suites_requested", []))

    output_csv = args.output
    if output_csv is None:
        output_csv = output_root / "matrix-comparison-unified.csv"
    output_csv.parent.mkdir(parents=True, exist_ok=True)

    suite_runs = summary.get("suite_runs", [])
    run_lookup: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for item in suite_runs:
        engine = str(item.get("engine", ""))
        suite = str(item.get("suite", ""))
        run_lookup[(engine, suite)] = item

    metric_table: Dict[Tuple[str, str], Dict[str, Any]] = {}

    def put_metric(suite_name: str, metric_name: str, engine_name: str, value: Any) -> None:
        key = (suite_name, metric_name)
        if key not in metric_table:
            metric_table[key] = {}
        metric_table[key][engine_name] = value

    for engine in engines:
        for suite in suites:
            run_item = run_lookup.get((engine, suite))
            if run_item:
                put_metric(suite, "matrix.status", engine, run_item.get("status"))
                put_metric(suite, "matrix.exit_code", engine, run_item.get("exit_code"))
                put_metric(
                    suite,
                    "matrix.duration_seconds",
                    engine,
                    run_item.get("duration_seconds"),
                )

            result_file = pick_suite_json_file(output_root, engine, suite)
            if result_file is None:
                continue

            if engine == "scratchbird":
                try:
                    provenance = validate_scratchbird_result_provenance(result_file)
                except ValueError as exc:
                    print(f"error: {exc}", file=sys.stderr)
                    return 3
                put_metric(
                    suite,
                    "artifact.provenance_json",
                    engine,
                    str(provenance_file_for_result(result_file).relative_to(output_root)),
                )
                put_metric(
                    suite,
                    "artifact.scratchbird_binary_sha256",
                    engine,
                    (((provenance.get("scratchbird_runtime") or {}).get("server_binary") or {}).get("sha256")),
                )

            put_metric(
                suite,
                "artifact.result_json",
                engine,
                str(result_file.relative_to(output_root)),
            )

            try:
                payload = load_json(result_file)
            except Exception as exc:  # pragma: no cover - safety path
                put_metric(suite, "artifact.parse_error", engine, str(exc))
                continue

            extracted = extract_suite_metrics(suite, payload)
            for metric_name, metric_value in extracted.items():
                put_metric(suite, metric_name, engine, metric_value)

    rows = sorted(
        metric_table.items(),
        key=lambda item: (suite_sort_key(item[0][0], suites), item[0][1]),
    )

    fieldnames = ["run_id", "suite", "metric"] + engines
    with output_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for (suite, metric), values in rows:
            row = {
                "run_id": summary.get("run_id", ""),
                "suite": suite,
                "metric": metric,
            }
            for engine in engines:
                row[engine] = as_csv_value(values.get(engine, ""))
            writer.writerow(row)

    print(output_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
