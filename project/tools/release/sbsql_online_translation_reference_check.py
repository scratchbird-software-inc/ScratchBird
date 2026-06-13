#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Compare SBsql seeded translations with online translation references.

This is an external verification utility, not a release-pack generation input.
It reads the generated online-translation verification corpus and writes an
evidence JSON to a caller-provided path. Raw online responses are never written
into the public resource pack by the generator.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_PACK_REL = "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"
CORPUS_REL = "resources/conformance/online-translation-verification-corpus.json"
MYMEMORY_ENDPOINT = "https://api.mymemory.translated.net/get"


def fail(message: str) -> None:
    print(message)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def normalize_tokens(text: str) -> set[str]:
    return {
        token.lower()
        for token in re.findall(r"[\wÀ-ÿ]+", text, flags=re.UNICODE)
        if len(token) > 1
    }


def token_overlap(lhs: str, rhs: str) -> float:
    left = normalize_tokens(lhs)
    right = normalize_tokens(rhs)
    if not left or not right:
        return 0.0
    return len(left & right) / len(left | right)


def query_mymemory(source_text: str, language_pair: str, timeout: float) -> dict[str, Any]:
    query = urllib.parse.urlencode({"q": source_text, "langpair": language_pair})
    url = f"{MYMEMORY_ENDPOINT}?{query}"
    request = urllib.request.Request(url, headers={"User-Agent": "ScratchBird-public-source-review/1.0"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    translated_text = str(payload.get("responseData", {}).get("translatedText", ""))
    return {
        "provider": "mymemory",
        "provider_url": MYMEMORY_ENDPOINT,
        "language_pair": language_pair,
        "response_status": payload.get("responseStatus"),
        "response_details": payload.get("responseDetails"),
        "translated_text": translated_text,
        "translated_text_sha256": sha256_text(translated_text),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--pack-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provider", choices=["mymemory"], default="mymemory")
    parser.add_argument("--limit-per-language", type=int, default=8)
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    parser.add_argument("--sleep-seconds", type=float, default=0.25)
    parser.add_argument("--minimum-token-overlap", type=float, default=0.0)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    pack_root = (args.pack_root if args.pack_root else repo_root / DEFAULT_PACK_REL).resolve()
    corpus_path = pack_root / CORPUS_REL
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    pairs = corpus.get("provider_policy", {}).get("supported_language_pairs", {})
    if not pairs:
        fail("online translation corpus does not define supported language pairs")

    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    per_language_count: dict[str, int] = {}
    for case in corpus.get("cases", []):
        by_tag = {row.get("exact_tag"): row for row in case.get("translations", [])}
        for exact_tag, language_pair in pairs.items():
            if per_language_count.get(exact_tag, 0) >= args.limit_per_language:
                continue
            seeded = by_tag.get(exact_tag, {})
            seeded_text = str(seeded.get("localized_template", ""))
            if args.provider == "mymemory":
                external = query_mymemory(str(case.get("provider_query_text", "")), language_pair, args.timeout_seconds)
            else:
                fail(f"unsupported provider: {args.provider}")
            overlap = token_overlap(seeded_text, str(external.get("translated_text", "")))
            status = "reference_captured"
            if not external.get("translated_text"):
                status = "provider_returned_empty_translation"
                failures.append(f"{case.get('case_id')}:{exact_tag}:empty")
            elif overlap < args.minimum_token_overlap:
                status = "below_overlap_threshold"
                failures.append(f"{case.get('case_id')}:{exact_tag}:overlap={overlap:.3f}")
            rows.append(
                {
                    "case_id": case.get("case_id"),
                    "exact_tag": exact_tag,
                    "source_text": case.get("source_text"),
                    "seeded_text": seeded_text,
                    "seeded_text_sha256": sha256_text(seeded_text),
                    "external": external,
                    "token_overlap": round(overlap, 4),
                    "status": status,
                }
            )
            per_language_count[exact_tag] = per_language_count.get(exact_tag, 0) + 1
            if args.sleep_seconds > 0:
                time.sleep(args.sleep_seconds)

    evidence = {
        "schema_version": "sbsql.online_translation_reference_check.v1",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repo_root": str(repo_root),
        "pack_root": str(pack_root),
        "corpus_sha256": sha256_text(corpus_path.read_text(encoding="utf-8")),
        "provider": args.provider,
        "release_pack_generation_uses_network": False,
        "raw_online_responses_written_to_public_pack": False,
        "limit_per_language": args.limit_per_language,
        "minimum_token_overlap": args.minimum_token_overlap,
        "row_count": len(rows),
        "per_language_count": per_language_count,
        "failure_count": len(failures),
        "failures": failures,
        "rows": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "sbsql_online_translation_reference_check=completed "
        f"rows={len(rows)} failures={len(failures)} output={args.output}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
