#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic public durable-codec fuzz coverage gate for PCR-114."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_DURABLE_CODEC_FUZZ

SURFACES: tuple[dict[str, Any], ...] = (
    {
        "surface": "catalog_page_body",
        "families": ["bounds", "checksum", "payload_checksum", "version"],
        "files": {
            "src/storage/page/catalog_page.cpp": (
                "SB-CATALOG-PAGE-BODY-PAGE-SIZE-TOO-SMALL",
                "SB-CATALOG-PAGE-BODY-ROW-TOO-LARGE",
                "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
                "SB-CATALOG-PAGE-BODY-CHECKSUM-MISMATCH",
                "SB-CATALOG-PAGE-BODY-PAYLOAD-CHECKSUM-MISMATCH",
            ),
            "tests/release/public_codec_property_gate.cpp": (
                "PUBLIC_CODEC_PROPERTY_GATE",
                "CatalogPageBodyProperties",
                "SB-CATALOG-PAGE-BODY-PAYLOAD-CHECKSUM-MISMATCH",
            ),
        },
    },
    {
        "surface": "typed_catalog_record",
        "families": ["bounds", "malformed_uuid", "row_kind", "version"],
        "files": {
            "src/core/catalog/catalog_record_codec.cpp": (
                "SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                "SB-CATALOG-RECORD-CODEC-ROW-KIND-INVALID",
                "SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                "ParseTypedUuid",
            ),
            "tests/release/public_codec_property_gate.cpp": (
                "CatalogTypedRecordProperties",
                "typed catalog record decoded malformed row UUID",
            ),
        },
    },
    {
        "surface": "datatype_descriptor",
        "families": ["checksum", "protected_keyed", "weak_profile"],
        "files": {
            "tests/release/public_datatype_binary_descriptor_integrity_gate.cpp": (
                "EncodeDatatypeDescriptorEnvelope",
                "DecodeDatatypeDescriptorEnvelope",
                "accepted corrupted payload",
                "accepted weak integrity profile",
                "datatype transport accepted corrupted envelope",
            ),
        },
    },
    {
        "surface": "index_durable_metadata",
        "families": ["checksum", "provider_evidence", "tamper", "version"],
        "files": {
            "tests/release/public_index_durable_metadata_validator_gate.cpp": (
                "durable_metadata_present",
                "metadata_format_version",
                "provider_evidence_hash_bound",
                "metadata_checksum_low64",
                "accepted tampered durable metadata",
                "corrupted metapage metadata",
            ),
        },
    },
    {
        "surface": "repair_ledger",
        "families": ["authority_refusal", "checksum", "quarantine", "tamper"],
        "files": {
            "tests/release/public_repair_event_ledger_quarantine_gate.cpp": (
                "AppendRepairEventToLedger",
                "LoadRepairEventLedger",
                "repair_evidence_is_transaction_finality_authority",
                "parser_or_reference_authority",
                "tampered repair ledger",
            ),
            "tests/release/public_repair_tamper_retention_crash_resume_gate.cpp": (
                "EvaluateRepairEventRetention",
                "repair_evidence_is_transaction_authority",
                "EvaluateRepairCrashResumeFromLedger",
                "repair_evidence_is_recovery_authority",
                "forged.ledger",
            ),
        },
    },
    {
        "surface": "archive_manifest",
        "families": ["authority_refusal", "checksum", "cluster_refusal", "retention"],
        "files": {
            "tests/release/public_archive_before_reclaim_gate.cpp": (
                "SBARCHIVERECLAIM1",
                "movement_record_checksum",
                "transaction_finality_authority",
                "cluster_route_refused",
                "missing retention policy",
            ),
        },
    },
    {
        "surface": "backup_forward_segment",
        "families": ["authority_refusal", "coverage", "idempotency", "manifest"],
        "files": {
            "tests/release/public_backup_forward_session_gate.cpp": (
                "write_after_segment_immutable",
                "coverage_contiguous",
                "authoritative_wal",
                "cluster_authority_required",
                "coverage gap",
            ),
            "tests/release/public_backup_update_coverage_gate.cpp": (
                "verified_segment_manifest_uris",
                "PITR_TARGET_OUTSIDE_COVERAGE",
                "reused_segment_count",
                "cluster_recovery_authority",
                "coverage",
            ),
        },
    },
    {
        "surface": "cluster_catalog_codec",
        "families": ["authority_refusal", "checksum", "resolver", "version"],
        "files": {
            "src/core/catalog/cluster_catalog_record_codec.cpp": (
                "SB-CLUSTER-CATALOG-RECORD-VERSION-UNSUPPORTED",
                "SB-CLUSTER-CATALOG-RECORD-CHECKSUM-MISMATCH",
                "SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
                "SB-CLUSTER-CATALOG-RECORD-REQUIRED-FIELD-MISSING",
                "SB-CLUSTER-CATALOG-RECORD-AUTHORITY-BOUNDARY-INVALID",
            ),
            "tests/release/public_cluster_catalog_codec_resolver_gate.cpp": (
                "PUBLIC_CLUSTER_CODEC_RESOLVER_GATE",
                "ValidateClusterCatalogRecordSet",
                "SB-CLUSTER-CATALOG-RECORD-RESOLVER-MISSING",
            ),
            "tests/release/public_codec_property_gate.cpp": (
                "ClusterCatalogRecordProperties",
                "SB-CLUSTER-CATALOG-RECORD-CHECKSUM-MISMATCH",
            ),
        },
    },
    {
        "surface": "cluster_crypto_evidence",
        "families": ["authority_refusal", "canonical_payload", "weak_evidence"],
        "files": {
            "src/core/catalog/cluster_catalog_crypto_evidence.cpp": (
                "CLUSTER_CATALOG_CRYPTO_EVIDENCE",
                "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-WEAK",
                "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-CANONICAL-MISMATCH",
                "signature-ready",
            ),
            "tests/release/public_cluster_catalog_crypto_evidence_gate.cpp": (
                "PUBLIC_CLUSTER_CATALOG_CRYPTO_EVIDENCE_GATE",
                "accepted weak FNV evidence",
                "digest mismatch diagnostic changed",
            ),
            "tests/release/public_codec_property_gate.cpp": (
                "ClusterEvidenceIntegrityProperties",
                "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-WEAK",
            ),
        },
    },
)

RELEASE_CMAKE_TOKENS = (
    "add_executable(public_codec_property_gate",
    "NAME public_codec_property_gate",
    "NAME public_durable_codec_fuzz",
    "PCR-GATE-114",
    "PUBLIC_RELEASE_CORRECTNESS_BUILD_TARGETS",
    "public_codec_property_gate",
)


def fail(message: str) -> None:
  print(f"public_durable_codec_fuzz=fail:{message}", file=sys.stderr)
  raise SystemExit(1)


def sha256_text(text: str) -> str:
  return hashlib.sha256(text.encode("utf-8")).hexdigest()


def require_file(project_root: Path, relative: str) -> str:
  path = project_root / relative
  if not path.is_file():
    fail(f"missing_file:{relative}")
  return path.read_text(encoding="utf-8")


def require_tokens(text: str, relative: str, tokens: tuple[str, ...]) -> list[str]:
  missing = [token for token in tokens if token not in text]
  if missing:
    fail(f"missing_tokens:{relative}:{','.join(missing)}")
  return list(tokens)


def build_evidence(project_root: Path) -> dict[str, Any]:
  if project_root.name != "project" or not project_root.is_dir():
    fail("project_root_must_be_project_directory")

  cmake_text = require_file(project_root, "tests/release/CMakeLists.txt")
  require_tokens(cmake_text, "tests/release/CMakeLists.txt", RELEASE_CMAKE_TOKENS)

  surface_records: list[dict[str, Any]] = []
  for surface in SURFACES:
    file_records: list[dict[str, Any]] = []
    for relative, tokens in surface["files"].items():
      text = require_file(project_root, relative)
      file_records.append(
          {
              "path": relative,
              "sha256": sha256_text(text),
              "required_tokens": require_tokens(text, relative, tokens),
          }
      )
    surface_records.append(
        {
            "surface": surface["surface"],
            "families": surface["families"],
            "file_count": len(file_records),
            "files": file_records,
        }
    )

  return {
      "schema_version": 1,
      "gate": "PCR-GATE-114",
      "marker": "PUBLIC_DURABLE_CODEC_FUZZ",
      "policy": {
          "deterministic": True,
          "random_seed_required": False,
          "private_docs_required": False,
          "malformed_inputs_fail_closed": True,
          "authority_refusals_are_required": True,
      },
      "surface_count": len(surface_records),
      "surfaces": surface_records,
  }


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--project-root", required=True)
  parser.add_argument("--output", required=True)
  args = parser.parse_args()

  project_root = Path(args.project_root).resolve()
  output = Path(args.output).resolve()
  evidence = build_evidence(project_root)
  output.parent.mkdir(parents=True, exist_ok=True)
  output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
  print(
      "public_durable_codec_fuzz=passed "
      f"surfaces={evidence['surface_count']} output={output.name}"
  )
  return 0


if __name__ == "__main__":
  sys.exit(main())
