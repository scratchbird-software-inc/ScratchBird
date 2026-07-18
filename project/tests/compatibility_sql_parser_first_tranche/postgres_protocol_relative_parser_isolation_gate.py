#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Prove protocol-related parsers do not depend on another parser family."""

from __future__ import annotations

import argparse
import json
import pathlib
import re


FAMILIES = ("cockroachdb", "yugabytedb", "xtdb")
FOREIGN_EDGE = re.compile(
    r"sbl_postgresql_parser_pipeline|"
    r"src/parsers/compatibility/postgresql|"
    r"postgresql_worker_session|"
    r"parser::postgresql|"
    r"ServePostgresql",
    re.IGNORECASE,
)
SEMANTIC_CODEC_EDGE = re.compile(
    r"\b(?:ParseStatement|ParseResult|DialectProfile|MappingDisposition|"
    r"sblr_envelope|statement_family|operation_family|RenderStatement)\b"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", required=True, type=pathlib.Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    parser_root = repo_root / "project/src/parsers/compatibility"

    family_evidence: list[dict[str, object]] = []
    for family in FAMILIES:
        root = parser_root / family
        sources = sorted(
            path for path in root.iterdir()
            if path.suffix in {".cpp", ".hpp", ".txt"}
        )
        combined = "\n".join(path.read_text(encoding="utf-8") for path in sources)
        match = FOREIGN_EDGE.search(combined)
        require(match is None, f"{family} retains foreign parser edge: {match.group(0) if match else ''}")

        worker_stem = f"{family}_worker_session"
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        main_source = (root / "main.cpp").read_text(encoding="utf-8")
        require((root / f"{worker_stem}.hpp").is_file(), f"{family} worker header missing")
        require((root / f"{worker_stem}.cpp").is_file(), f"{family} worker implementation missing")
        require(f"{worker_stem}.cpp" in cmake, f"{family} worker absent from own pipeline")
        require("sbl_compatibility_parser_common" in cmake,
                f"{family} neutral framing dependency missing")
        require(f'#include "{worker_stem}.hpp"' in main_source,
                f"{family} executable does not include own worker")
        require(f"scratchbird::parser::{family}::Serve" in main_source,
                f"{family} executable does not invoke own session namespace")
        family_evidence.append({
            "family": family,
            "postgresql_parser_dependency_count": 0,
            "own_worker_session": True,
            "own_parser_pipeline": f"sbl_{family}_parser_pipeline",
            "neutral_frame_codec": "pgwire_frame_codec",
        })

    common_root = parser_root / "common"
    codec_files = [common_root / "pgwire_frame_codec.hpp",
                   common_root / "pgwire_frame_codec.cpp"]
    require(all(path.is_file() for path in codec_files), "neutral frame codec files missing")
    codec_text = "\n".join(path.read_text(encoding="utf-8") for path in codec_files)
    semantic_match = SEMANTIC_CODEC_EDGE.search(codec_text)
    require(semantic_match is None,
            f"neutral frame codec contains parser-family semantics: "
            f"{semantic_match.group(0) if semantic_match else ''}")
    require("pgwire_frame_codec.cpp" in
            (common_root / "CMakeLists.txt").read_text(encoding="utf-8"),
            "neutral frame codec is not in the neutral library")

    # Scanner effectiveness is part of the proof, not an assumed property.
    negative_fixtures = (
        "target_link_libraries(x sbl_postgresql_parser_pipeline)",
        '#include "postgresql_worker_session.hpp"',
        "scratchbird::parser::postgresql::ServePostgresqlWorkerSession(fd)",
    )
    require(all(FOREIGN_EDGE.search(fixture) for fixture in negative_fixtures),
            "foreign-edge scanner did not reject every injected fixture")

    evidence = {
        "gate": "postgres_protocol_relative_parser_isolation_gate",
        "status": "passed",
        "families": family_evidence,
        "postgresql_parser_dependency_count": 0,
        "neutral_codec_semantic_edge_count": 0,
        "negative_fixture_count": len(negative_fixtures),
        "global_shared_semantic_engine_audit_required": True,
        "parser_transaction_finality_authority": False,
        "mga_transaction_authority": "scratchbird_engine",
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                                  encoding="utf-8")
    print("postgres_protocol_relative_parser_isolation_gate=passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"postgres_protocol_relative_parser_isolation_gate: {exc}")
        raise SystemExit(1)
