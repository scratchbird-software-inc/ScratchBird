#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify the generated SBsql language resource pack.

This verifier is read-only for the checked repository. It rejects malformed,
tampered, unsigned, stale, revoked, incompatible, ambiguous, duplicate,
oversized, and unsafe language resource packs. It also proves deterministic
generation against the public generator without reading draft documentation or
using the network.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable


DEFAULT_PACK_REL = "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"
SEED_PACK_REL = "project/resources/seed-packs/initial-resource-pack"
GENERATOR_REL = "project/tools/sb_parser_gen/generate_sbsql_language_resource_pack.py"
DRIVER_SURFACE_MANIFEST = "project/drivers/language/sbsql_language_surface_manifest.json"
PACK_SCHEMA_VERSION = "sbsql.language_resource_pack.v1"
MANIFEST_SCHEMA_VERSION = "sbsql.language_resource_pack_manifest.v1"
SIGNATURE_SCHEMA_VERSION = "sbsql.language_resource_pack_signature.v1"
EXPECTED_EXACT_PROFILES = ["en-US", "fr-FR", "de-DE", "it-IT", "es-ES"]
EXPECTED_COMMON_RESOURCE_IDENTITY = "sbsql.common_resource_pack.v1"
EXPECTED_DIALECT_PROFILE_UUID = "sbsql.v3"
EXPECTED_TOPOLOGY_PROFILE_UUID = "topology.sbsql.canonical.v1"
EXPECTED_AUTHORITY_FLAGS = (
    "local_sblr_uuid_streams_are_untrusted",
    "server_revalidates_sblr_uuid_descriptor_authorization_policy_and_mga",
    "normalization_before_uuid_resolution",
)
ALLOWED_EXTRA_ROOT_FILES = {"manifest.sblrp.json", "manifest.sblrp.sig", "hashes.sha256"}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def canonical_json_bytes(payload: Any) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def fnv1a64_bytes(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_hashes(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        parts = line.split("  ", 1)
        if len(parts) != 2:
            fail(f"{path}: invalid hashes.sha256 line {line_number}")
        digest, rel_path = parts
        if not digest.startswith("sha256:"):
            fail(f"{path}: invalid sha256 digest on line {line_number}")
        if rel_path in result:
            fail(f"{path}: duplicate hash entry {rel_path}")
        result[rel_path] = digest
    return result


def load_generator(repo_root: Path) -> Any:
    generator_path = repo_root / GENERATOR_REL
    spec = importlib.util.spec_from_file_location("sb_lrp_generator", generator_path)
    if spec is None or spec.loader is None:
        fail(f"cannot load generator: {generator_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def collect_pack_files(pack_root: Path) -> set[str]:
    return {
        path.relative_to(pack_root).as_posix()
        for path in pack_root.rglob("*")
        if path.is_file()
    }


def validate_manifest_structure(pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema_version") != MANIFEST_SCHEMA_VERSION:
        errors.append("manifest schema_version mismatch")
    if manifest.get("pack_schema_version") != PACK_SCHEMA_VERSION:
        errors.append("manifest pack_schema_version mismatch")
    if manifest.get("resource_identity") != EXPECTED_COMMON_RESOURCE_IDENTITY:
        errors.append("manifest resource_identity mismatch")
    if manifest.get("dialect_profile_uuid") != EXPECTED_DIALECT_PROFILE_UUID:
        errors.append("manifest dialect_profile_uuid mismatch")
    if manifest.get("topology_profile_uuid") != EXPECTED_TOPOLOGY_PROFILE_UUID:
        errors.append("manifest topology_profile_uuid mismatch")
    authority = manifest.get("authority", {})
    for flag in EXPECTED_AUTHORITY_FLAGS:
        if authority.get(flag) is not True:
            errors.append(f"manifest authority flag must be true: {flag}")
    profiles = manifest.get("profiles")
    if not isinstance(profiles, list):
        return errors + ["manifest profiles must be a list"]
    exact_tags = [str(profile.get("exact_tag", "")) for profile in profiles]
    if exact_tags != EXPECTED_EXACT_PROFILES:
        errors.append(f"manifest exact profiles drifted: {exact_tags}")
    if len(exact_tags) != len(set(exact_tags)):
        errors.append("manifest duplicate exact profile tags")
    for profile in profiles:
        exact_tag = profile.get("exact_tag")
        if "*" in str(exact_tag):
            errors.append(f"wildcard exact profile is forbidden: {exact_tag}")
        if profile.get("release_channel") in {"revoked", "removed", "expired"}:
            errors.append(f"{exact_tag}: revoked/removed/expired profile cannot be admitted")
        if exact_tag == "en-US":
            if profile.get("release_channel") != "release_supported":
                errors.append("en-US profile must be release_supported")
        elif profile.get("release_channel") != "beta":
            errors.append(f"{exact_tag}: non-English initial profile must be beta")
        resource_path = profile.get("resource_path")
        if not isinstance(resource_path, str) or not (pack_root / resource_path).is_file():
            errors.append(f"{exact_tag}: profile resource path missing")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        errors.append("manifest files must be a non-empty list")
    else:
        paths = [str(row.get("path", "")) for row in files]
        if len(paths) != len(set(paths)):
            errors.append("manifest duplicate file paths")
        for row in files:
            rel_path = str(row.get("path", ""))
            if rel_path.startswith("/") or ".." in Path(rel_path).parts:
                errors.append(f"unsafe manifest path: {rel_path}")
            if rel_path.startswith("docs/documentation/draft/"):
                errors.append("draft documentation path leaked into language pack manifest")
            path = pack_root / rel_path
            if not path.is_file():
                errors.append(f"manifest file missing: {rel_path}")
                continue
            observed = sha256_file(path)
            if row.get("sha256") != observed:
                errors.append(f"manifest sha mismatch: {rel_path}")
            if row.get("size_bytes") != path.stat().st_size:
                errors.append(f"manifest size mismatch: {rel_path}")
    return errors


def validate_hashes_and_signature(pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    hash_path = pack_root / "hashes.sha256"
    sig_path = pack_root / "manifest.sblrp.sig"
    manifest_path = pack_root / "manifest.sblrp.json"
    if not hash_path.is_file():
        return ["hashes.sha256 missing"]
    if not sig_path.is_file():
        return ["manifest.sblrp.sig missing"]
    hashes = parse_hashes(hash_path)
    expected_paths = {str(row["path"]) for row in manifest.get("files", [])}
    expected_paths.add("manifest.sblrp.json")
    if set(hashes) != expected_paths:
        errors.append("hashes.sha256 path set does not match manifest resources plus manifest")
    for rel_path, digest in hashes.items():
        path = pack_root / rel_path
        if not path.is_file():
            errors.append(f"hash entry missing file: {rel_path}")
        elif sha256_file(path) != digest:
            errors.append(f"hashes.sha256 digest mismatch: {rel_path}")
    try:
        signature = read_json(sig_path)
    except (OSError, json.JSONDecodeError) as exc:
        return errors + [f"signature unreadable: {exc}"]
    if signature.get("schema_version") != SIGNATURE_SCHEMA_VERSION:
        errors.append("signature schema_version mismatch")
    if signature.get("signed_manifest_sha256") != sha256_file(manifest_path):
        errors.append("signature manifest hash mismatch")
    if signature.get("signed_hashes_sha256") != sha256_file(hash_path):
        errors.append("signature hashes hash mismatch")
    expected_transcript = sha256_bytes(
        canonical_json_bytes(
            {
                "hashes": signature.get("signed_hashes_sha256"),
                "key_id": signature.get("key_id"),
                "manifest": signature.get("signed_manifest_sha256"),
            }
        )
    )
    if signature.get("transcript_sha256") != expected_transcript:
        errors.append("signature transcript hash mismatch")
    return errors


def validate_no_extra_files(pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    declared = {str(row["path"]) for row in manifest.get("files", [])}
    declared |= ALLOWED_EXTRA_ROOT_FILES
    actual = collect_pack_files(pack_root)
    extra = sorted(actual - declared)
    missing = sorted(declared - actual)
    errors: list[str] = []
    if extra:
        errors.append(f"pack contains undeclared extra files: {extra[:5]}")
    if missing:
        errors.append(f"pack missing declared root files: {missing[:5]}")
    return errors


def validate_canonical_resources(pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    system = read_json(pack_root / "resources/canonical/system-object-name-registry.json")
    dialect = read_json(pack_root / "resources/canonical/sbsql-dialect-baseline.json")
    language_profiles = {
        tag: read_json(pack_root / f"resources/languages/{tag}/language-profile.json")
        for tag in EXPECTED_EXACT_PROFILES
    }
    corpus_rows = [
        json.loads(line)
        for line in (pack_root / "resources/canonical/translation-source-corpus.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    system_entries = system.get("entries", [])
    if not isinstance(system_entries, list) or not system_entries:
        errors.append("system object registry entries missing")
    by_source_key = {entry.get("source_key"): entry for entry in system_entries}
    if by_source_key.get("user_schema", {}).get("canonical_full_name") != "user schema":
        errors.append("system object registry must include user schema as a full distinct name")
    if by_source_key.get("user", {}).get("canonical_full_name") != "user":
        errors.append("system object registry must include user security identity separately from user schema")
    for entry in system_entries:
        for field in ("entry_id", "object_kind", "object_class", "canonical_full_name", "plural_full_name", "system_path", "collision_group", "translation_context"):
            if not entry.get(field):
                errors.append(f"system object entry missing {field}: {entry.get('source_key')}")
    surfaces = dialect.get("surfaces", [])
    if len(surfaces) != manifest.get("registry_row_count"):
        errors.append("dialect surface count does not match manifest registry_row_count")
    surface_ids = [row.get("surface_id") for row in surfaces]
    if len(surface_ids) != len(set(surface_ids)):
        errors.append("dialect baseline duplicate surface_id")
    if dialect.get("registry", {}).get("cpp_row_count") != manifest.get("registry_row_count"):
        errors.append("dialect registry cpp row count does not match manifest")
    source_ids = {entry.get("entry_id") for entry in system_entries}
    source_ids |= {row.get("surface_id") for row in surfaces}
    source_ids |= {
        "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
        "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH",
        "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
        "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
    }
    corpus_ids = [row.get("record_id") for row in corpus_rows]
    if len(corpus_ids) != len(set(corpus_ids)):
        errors.append("translation source corpus duplicate record_id")
    for row in corpus_rows:
        if row.get("source_id") not in source_ids:
            errors.append(f"translation source corpus source_id is not canonical: {row.get('source_id')}")
        if not row.get("translation_context"):
            errors.append(f"translation source corpus row lacks context: {row.get('record_id')}")
    for tag, profile in language_profiles.items():
        if profile.get("exact_tag") != tag:
            errors.append(f"{tag}: language profile exact_tag mismatch")
        if profile.get("translation_count") != len(corpus_rows):
            errors.append(f"{tag}: translation_count does not match source corpus")
        translations = profile.get("translations", [])
        ids = [row.get("record_id") for row in translations]
        if len(ids) != len(set(ids)):
            errors.append(f"{tag}: duplicate translation record_id")
        if set(ids) != set(corpus_ids):
            errors.append(f"{tag}: translation rows do not exactly cover source corpus")
        if tag == "en-US" and profile.get("release_channel") != "release_supported":
            errors.append("en-US language profile release channel drifted")
        if tag != "en-US" and profile.get("release_channel") != "beta":
            errors.append(f"{tag}: beta language profile release channel drifted")
        if tag != "en-US" and profile.get("native_review_state") != "native_technical_review_required_before_release_support":
            errors.append(f"{tag}: native review status must be explicit")
    return errors


def validate_auxiliary_resources(pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    predictive = read_json(pack_root / "resources/predictive/predictive-grammar.json")
    topology = read_json(pack_root / "resources/topology/topology-profiles.json")
    unicode_policy = read_json(pack_root / "resources/unicode/unicode-policy.json")
    resolver = read_json(pack_root / "resources/resolver/resolver-policy.json")
    conformance = read_json(pack_root / "resources/conformance/conformance-corpus.json")
    if predictive.get("no_database_object_names") is not True or predictive.get("no_uuid_values") is not True:
        errors.append("predictive resource must not expose database object names or UUIDs")
    states = predictive.get("states", [])
    if not isinstance(states, list) or len(states) != manifest.get("registry_row_count"):
        errors.append("predictive states must exactly cover the registry row count")
    if predictive.get("max_table_entries") != len(states):
        errors.append("predictive max_table_entries must equal state count")
    for state in states[:50]:
        if "uuid" in json.dumps(state, sort_keys=True).lower():
            errors.append("predictive state leaks uuid material")
            break
    if topology.get("normalization_stage") != "stream_analysis_before_uuid_resolution":
        errors.append("topology normalization must happen before UUID resolution")
    topo_tags = [profile.get("exact_tag") for profile in topology.get("profiles", [])]
    if topo_tags != EXPECTED_EXACT_PROFILES:
        errors.append("topology exact profiles drifted")
    if unicode_policy.get("hidden_object_disclosure_allowed") is not False:
        errors.append("unicode policy must not allow hidden object disclosure")
    if resolver.get("authorized_schema_path_resolution_required") is not True:
        errors.append("resolver must require authorized schema path resolution")
    if resolver.get("authorized_uuid_to_path_required") is not True:
        errors.append("resolver must require authorized UUID-to-path resolution")
    coverage = conformance.get("coverage", {})
    if coverage.get("registry_surface_count") != manifest.get("registry_row_count"):
        errors.append("conformance corpus registry coverage drifted")
    return errors


def validate_seed_pack_index(repo_root: Path, pack_root: Path) -> list[str]:
    seed_root = repo_root / SEED_PACK_REL
    errors: list[str] = []
    manifest_rows = read_csv_rows(seed_root / "RESOURCE_SEED_MANIFEST.csv")
    families = {row.get("seed_family"): row for row in manifest_rows}
    for family in (
        "sbsql_language_resource_pack",
        "sbsql_language_resource_pack_artifacts",
        "sbsql_language_resource_pack_provenance",
    ):
        if family not in families:
            errors.append(f"seed manifest missing {family}")
        elif families[family].get("status") != "specified":
            errors.append(f"seed manifest family is not specified: {family}")
    artifact_rows = read_csv_rows(seed_root / "RESOURCE_SEED_ARTIFACTS.csv")
    artifact_by_path = {row.get("canonical_path"): row for row in artifact_rows}
    for path in sorted(pack_root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(seed_root).as_posix()
        row = artifact_by_path.get(rel)
        if row is None:
            errors.append(f"seed artifact index missing pack file: {rel}")
            continue
        data = path.read_bytes()
        if row.get("content_hash") != fnv1a64_bytes(data):
            errors.append(f"seed artifact FNV mismatch: {rel}")
        if row.get("content_size_bytes") != str(len(data)):
            errors.append(f"seed artifact size mismatch: {rel}")
    return errors


def validate_driver_manifest(repo_root: Path, pack_root: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    driver = read_json(repo_root / DRIVER_SURFACE_MANIFEST)
    metadata = driver.get("common_resource_pack_metadata", {})
    expected_path = DEFAULT_PACK_REL
    if metadata.get("resource_pack_path") != expected_path:
        errors.append("driver language surface manifest resource_pack_path mismatch")
    if metadata.get("resource_pack_manifest_sha256") != sha256_file(pack_root / "manifest.sblrp.json"):
        errors.append("driver language surface manifest pack manifest hash mismatch")
    if metadata.get("resource_pack_common_resource_hash") != manifest.get("common_resource_hash"):
        errors.append("driver language surface manifest common resource hash mismatch")
    if metadata.get("supported_exact_profiles") != EXPECTED_EXACT_PROFILES:
        errors.append("driver language surface manifest exact profile list mismatch")
    return errors


def validate_deterministic_generation(repo_root: Path, pack_root: Path) -> list[str]:
    generator = load_generator(repo_root)
    expected_files, _, _ = generator.build_files(repo_root)
    errors: list[str] = []
    for item in expected_files:
        observed_path = pack_root / item.rel_path
        if not observed_path.is_file():
            errors.append(f"deterministic generation missing file: {item.rel_path}")
            continue
        observed = observed_path.read_bytes()
        if observed != item.data:
            errors.append(f"deterministic generation drift: {item.rel_path}")
    expected_paths = {item.rel_path for item in expected_files}
    observed_paths = collect_pack_files(pack_root)
    extras = sorted(observed_paths - expected_paths)
    if extras:
        errors.append(f"deterministic generation has unexpected pack files: {extras[:5]}")
    return errors


def validate_pack(repo_root: Path, pack_root: Path, deterministic: bool = False) -> list[str]:
    errors: list[str] = []
    manifest_path = pack_root / "manifest.sblrp.json"
    if not manifest_path.is_file():
        return ["manifest.sblrp.json missing"]
    try:
        manifest = read_json(manifest_path)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"manifest unreadable: {exc}"]
    errors.extend(validate_manifest_structure(pack_root, manifest))
    errors.extend(validate_hashes_and_signature(pack_root, manifest))
    errors.extend(validate_no_extra_files(pack_root, manifest))
    if not errors:
        errors.extend(validate_canonical_resources(pack_root, manifest))
        errors.extend(validate_auxiliary_resources(pack_root, manifest))
    if pack_root == (repo_root / DEFAULT_PACK_REL).resolve():
        errors.extend(validate_seed_pack_index(repo_root, pack_root))
        errors.extend(validate_driver_manifest(repo_root, pack_root, manifest))
    if deterministic:
        errors.extend(validate_deterministic_generation(repo_root, pack_root))
    return errors


def mutate_json(path: Path, mutator: Callable[[Any], None]) -> None:
    payload = read_json(path)
    mutator(payload)
    path.write_text(json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


def run_corruption_oracles(repo_root: Path, pack_root: Path) -> list[str]:
    oracle_failures: list[str] = []
    cases: list[tuple[str, Callable[[Path], None]]] = [
        ("unsigned_missing_signature", lambda root: (root / "manifest.sblrp.sig").unlink()),
        ("tampered_language_profile", lambda root: mutate_json(root / "resources/languages/fr-FR/language-profile.json", lambda payload: payload.update({"display_name": "tampered"}))),
        ("stale_schema_version", lambda root: mutate_json(root / "manifest.sblrp.json", lambda payload: payload.update({"pack_schema_version": "stale"}))),
        ("duplicate_exact_profile", lambda root: mutate_json(root / "manifest.sblrp.json", lambda payload: payload["profiles"].append(dict(payload["profiles"][0])))),
        ("revoked_profile", lambda root: mutate_json(root / "manifest.sblrp.json", lambda payload: payload["profiles"][0].update({"release_channel": "revoked"}))),
        ("extra_file", lambda root: (root / "resources/extra.json").write_text("{}\n", encoding="utf-8")),
        ("unsafe_unicode_policy", lambda root: mutate_json(root / "resources/unicode/unicode-policy.json", lambda payload: payload.update({"hidden_object_disclosure_allowed": True}))),
        ("oversized_predictive_table", lambda root: mutate_json(root / "resources/predictive/predictive-grammar.json", lambda payload: payload.update({"max_table_entries": payload.get("max_table_entries", 0) + 1}))),
        ("ambiguous_topology", lambda root: mutate_json(root / "resources/topology/topology-profiles.json", lambda payload: payload.update({"normalization_stage": "after_uuid_resolution"}))),
    ]
    for name, mutator in cases:
        with tempfile.TemporaryDirectory(prefix=f"sbsql_lrp_{name}_") as tmp:
            temp_root = Path(tmp) / "sbsql-language-resource-pack"
            shutil.copytree(pack_root, temp_root)
            mutator(temp_root)
            errors = validate_pack(repo_root, temp_root, deterministic=False)
            if not errors:
                oracle_failures.append(f"corruption oracle did not fail: {name}")
    return oracle_failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--pack-root", type=Path)
    parser.add_argument("--check-generated", action="store_true")
    parser.add_argument("--check-corruption", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    pack_root = (args.pack_root if args.pack_root else repo_root / DEFAULT_PACK_REL).resolve()
    if not repo_root.is_dir():
        fail(f"repo root not found: {repo_root}")
    if not pack_root.is_dir():
        fail(f"pack root not found: {pack_root}")

    errors = validate_pack(repo_root, pack_root, deterministic=args.check_generated)
    if args.check_corruption and not errors:
        errors.extend(run_corruption_oracles(repo_root, pack_root))

    if errors:
        print(f"verify_sbsql_language_resource_pack=failed errors={len(errors)}", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    manifest = read_json(pack_root / "manifest.sblrp.json")
    print(
        "verify_sbsql_language_resource_pack=passed "
        f"profiles={len(manifest['profiles'])} registry_rows={manifest['registry_row_count']} "
        f"common_resource_hash={manifest['common_resource_hash']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
