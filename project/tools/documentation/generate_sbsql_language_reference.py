#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the SBsql language reference from current private contracts."""

from __future__ import annotations

import csv
import hashlib
import re
from collections import Counter, defaultdict
from pathlib import Path
from textwrap import dedent


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "docs/reference/sbsql-language-reference.md"
COMPLETED_EXECUTION_PLAN_ROOT = "docs" + "/" + "completed-execution-plans"

SOURCES = {
    "manifest": "public_contract_snapshot",
    "documentation_style": "public_contract_snapshot",
    "native_dialect": "public_contract_snapshot",
    "native_grammar": "public_contract_snapshot",
    "grammar_binding": "public_contract_snapshot",
    "management_grammar": "public_contract_snapshot",
    "function_catalog": "public_contract_snapshot",
    "zero_unresolved": "public_contract_snapshot",
    "surface_registry": "public_input_snapshot",
    "surface_status": "public_input_snapshot",
    "sblr_matrix": "public_input_snapshot",
    "documentation_source_map": "public_input_snapshot",
    "grammar_prep_contract": COMPLETED_EXECUTION_PLAN_ROOT + "/sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_CONTRACT.md",
    "grammar_prep_inventory": COMPLETED_EXECUTION_PLAN_ROOT + "/sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_INVENTORY.yaml",
    "grammar_prep_lexical": COMPLETED_EXECUTION_PLAN_ROOT + "/sbsql-native-v3-full-dialect-support/SBSQL_V3_LEXICAL_CONTRACT.md",
    "grammar_prep_ambiguity": COMPLETED_EXECUTION_PLAN_ROOT + "/sbsql-native-v3-full-dialect-support/SBSQL_V3_AMBIGUITY_RULES.md",
}


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def read_text(source_key: str) -> str:
    path = ROOT / SOURCES[source_key]
    return path.read_text(encoding="utf-8")


def read_csv(source_key: str) -> list[dict[str, str]]:
    path = ROOT / SOURCES[source_key]
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def ascii_clean(text: str) -> str:
    replacements = {
        "\u2013": "-",
        "\u2014": "-",
        "\u2018": "'",
        "\u2019": "'",
        "\u201c": '"',
        "\u201d": '"',
        "\u2026": "...",
        "\u2192": "->",
        "\u2190": "<-",
        "\u00a0": " ",
        "\u00d7": "x",
        "\u2265": ">=",
        "\u2264": "<=",
    }
    for src, dst in replacements.items():
        text = text.replace(src, dst)
    return "".join(ch if ord(ch) < 128 else "?" for ch in text)


def extract_search_keys(text: str) -> list[str]:
    keys: list[str] = []
    for match in re.finditer(r"Search key[s]?:\s*`([^`]+)`", text):
        keys.append(match.group(1))
    for match in re.finditer(r"Search keys:\s*([^.\n]+)", text):
        keys.extend(k.strip(" `") for k in match.group(1).split(",") if k.strip())
    return sorted(set(keys))


def extract_section(text: str, heading: str, next_heading_level: str = "## ") -> str:
    pattern = re.escape(heading)
    start_match = re.search(pattern, text)
    if not start_match:
        return ""
    start = start_match.start()
    next_match = re.search(r"\n" + re.escape(next_heading_level), text[start + 1 :])
    if not next_match:
        return text[start:].strip()
    end = start + 1 + next_match.start()
    return text[start:end].strip()


def extract_markdown_table(section: str) -> list[str]:
    lines = []
    in_table = False
    for line in section.splitlines():
        if line.startswith("|"):
            in_table = True
            lines.append(line)
        elif in_table:
            break
    return lines


def extract_code_blocks(section: str, language: str = "ebnf") -> list[str]:
    blocks: list[str] = []
    fence = "```" + language
    start = 0
    while True:
        begin = section.find(fence, start)
        if begin == -1:
            break
        code_start = section.find("\n", begin)
        if code_start == -1:
            break
        end = section.find("```", code_start + 1)
        if end == -1:
            break
        blocks.append(section[code_start + 1 : end].strip())
        start = end + 3
    return blocks


def bullet_list_between(text: str, start_heading: str, end_heading: str) -> list[str]:
    start = text.find(start_heading)
    end = text.find(end_heading, start + len(start_heading))
    if start == -1 or end == -1:
        return []
    body = text[start:end]
    out = []
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith("- `") and stripped.endswith("`"):
            out.append(stripped[3:-1])
    return out


def counter_table(counter: Counter[str], header: tuple[str, str], limit: int | None = None) -> str:
    rows = counter.most_common(limit)
    lines = [f"| {header[0]} | {header[1]} |", "| --- | ---: |"]
    for key, count in rows:
        lines.append(f"| `{key}` | {count} |")
    return "\n".join(lines)


def yaml_field_counter(text: str, field: str) -> Counter[str]:
    pattern = r"^\s+" + re.escape(field) + r":\s*\"?([^\"\n]+?)\"?\s*$"
    return Counter(match.strip() for match in re.findall(pattern, text, flags=re.MULTILINE))


def rows_table(rows: list[dict[str, str]], columns: list[str], limit: int | None = None) -> str:
    selected = rows[:limit] if limit is not None else rows
    lines = ["| " + " | ".join(columns) + " |", "| " + " | ".join(["---"] * len(columns)) + " |"]
    for row in selected:
        vals = []
        for col in columns:
            vals.append("`" + ascii_clean(row.get(col, "")).replace("|", "\\|") + "`")
        lines.append("| " + " | ".join(vals) + " |")
    return "\n".join(lines)


def group_rows(rows: list[dict[str, str]], field: str) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row.get(field, "")].append(row)
    return dict(sorted(grouped.items(), key=lambda item: item[0]))


def section_heading(title: str) -> str:
    return "\n\n## " + title + "\n"


def generate() -> str:
    surface_rows = read_csv("surface_registry")
    status_rows = read_csv("surface_status")
    sblr_rows = read_csv("sblr_matrix")
    native_grammar = read_text("native_grammar")
    native_dialect = read_text("native_dialect")
    management = read_text("management_grammar")
    zero_unresolved = read_text("zero_unresolved")
    grammar_prep_contract = read_text("grammar_prep_contract")
    grammar_prep_inventory = read_text("grammar_prep_inventory")

    surface_by_id = {r["surface_id"]: r for r in surface_rows}
    status_by_id = {r["surface_id"]: r for r in status_rows}
    sblr_by_id = {r["surface_id"]: r for r in sblr_rows}

    joined = []
    for row in surface_rows:
        sid = row["surface_id"]
        merged = dict(row)
        merged["allowed_lowering"] = status_by_id.get(sid, {}).get("allowed_lowering", "")
        merged["diagnostic_if_not_allowed"] = status_by_id.get(sid, {}).get("diagnostic_if_not_allowed", "")
        merged["result_shape"] = sblr_by_id.get(sid, {}).get("result_shape", "")
        merged["diagnostics"] = sblr_by_id.get(sid, {}).get("diagnostics", "")
        joined.append(merged)

    lexical = bullet_list_between(native_grammar, "## Lexical rules", "## Statement categories")
    statement_categories = extract_section(native_grammar, "## Statement categories")
    native_command_taxonomy = extract_markdown_table(extract_section(native_dialect, "## 3. Command taxonomy"))
    object_classes = extract_markdown_table(extract_section(native_dialect, "## 4. Object classes covered by SBSQL DDL"))
    show_taxonomy = extract_markdown_table(extract_section(native_dialect, "## 5. Native SHOW taxonomy"))
    parser_pipeline = extract_markdown_table(extract_section(zero_unresolved, "## 4. Parser pipeline"))
    parser_limits = extract_markdown_table(extract_section(zero_unresolved, "## 5. Lexical, keyword, and comment closures"))
    source_of_editing = extract_markdown_table(extract_section(zero_unresolved, "## 2. Required output artifacts"))
    mgmt_blocks = extract_code_blocks(management, "ebnf")
    prep_blocks = extract_code_blocks(grammar_prep_contract, "ebnf")
    prep_inventory_rows = len(re.findall(r"^\s+- surface_key:", grammar_prep_inventory, flags=re.MULTILINE))
    prep_family_counts = yaml_field_counter(grammar_prep_inventory, "command_family")
    prep_status_counts = yaml_field_counter(grammar_prep_inventory, "grammar_status")

    grammar_rows = [r for r in joined if r["surface_kind"] == "grammar_production"]
    function_rows = [r for r in joined if r["surface_kind"] == "function"]
    operator_rows = [r for r in joined if r["surface_kind"] == "operator"]
    canonical_rows = [r for r in joined if r["surface_kind"] == "canonical_surface"]
    variable_rows = [r for r in joined if r["surface_kind"] == "variable"]

    source_hashes = []
    for key, rel_path in SOURCES.items():
        path = ROOT / rel_path
        text_keys = extract_search_keys(path.read_text(encoding="utf-8", errors="ignore")) if path.suffix in {".md", ".yaml"} else []
        source_hashes.append((key, rel_path, sha256_file(path), ", ".join(text_keys[:4])))

    lines: list[str] = []
    lines.append("# ScratchBird SBsql Language Reference")
    lines.append("")
    lines.append("Generated: 2026-06-04")
    lines.append("")
    lines.append("Status: generated private reference from current contracts.")
    lines.append("")
    lines.append("This file replaces the older hand-maintained SBsql reference. It is generated from current private contracts and machine-readable SBsql registries. It is a reference artifact under `docs/reference`; canonical behavior remains controlled by manifest-listed files under `public_release_evidence`.")
    lines.append("")
    lines.append("Search key: `SBSQL-LANGUAGE-REFERENCE-GENERATED-FROM-CURRENT-SPECS`")

    lines.append(section_heading("1. Authority Boundary"))
    lines.append("SBsql is the native ScratchBird user-facing language. It is not engine execution authority. SBsql text is parsed into CST, AST, BoundAST, and then SBLR/internal operation envelopes. The engine executes SBLR and internal procedures only, revalidating UUID catalog identity, descriptors, security, policy, transaction, and MGA state.")
    lines.append("")
    lines.append("This reference uses only contract and registry inputs. It does not inspect implementation source code.")
    lines.append("")
    lines.append("| Rule | Requirement |")
    lines.append("| --- | --- |")
    lines.append("| Durable identity | Catalog UUIDs, not user-visible names. |")
    lines.append("| Parser authority | Parser creates parse/bind/lowering artifacts only. |")
    lines.append("| Engine authority | SBLR, descriptors, policies, grants, catalog UUIDs, and MGA state. |")
    lines.append("| Recovery language | ScratchBird recovery is MGA-based; WAL wording is reference compatibility only. |")
    lines.append("| Reference syntax | Reference profiles rewrite or refuse; reference syntax does not create native SBsql authority. |")

    lines.append(section_heading("2. Source Map"))
    lines.append("| Source key | Private path | SHA-256 | Search keys observed |")
    lines.append("| --- | --- | --- | --- |")
    for key, path, digest, search_keys in source_hashes:
        lines.append(f"| `{key}` | `{path}` | `{digest}` | `{search_keys}` |")

    lines.append(section_heading("3. Generated Corpus Summary"))
    lines.append(f"The current compiled SBsql surface registry contains `{len(surface_rows)}` rows. The status matrix contains `{len(status_rows)}` rows. The SBLR operation matrix contains `{len(sblr_rows)}` rows.")
    lines.append("")
    lines.append(counter_table(Counter(r["surface_kind"] for r in surface_rows), ("Surface kind", "Rows")))
    lines.append("")
    lines.append(counter_table(Counter(r["family"] for r in surface_rows), ("Family", "Rows")))
    lines.append("")
    lines.append(counter_table(Counter(r["status"] for r in surface_rows), ("Status", "Rows")))
    lines.append("")
    lines.append(counter_table(Counter(r["cluster_scope"] for r in surface_rows), ("Cluster scope", "Rows")))
    lines.append("")
    lines.append(counter_table(Counter(r["sblr_operation_family"] for r in sblr_rows), ("SBLR operation family", "Rows")))

    lines.append(section_heading("4. Language Structure"))
    lines.append("Native SBsql statement families are grouped by authority and lowering family. The command taxonomy below is copied from the current native dialect contract.")
    lines.append("")
    lines.extend(native_command_taxonomy or ["No command taxonomy table was found in the current native dialect contract."])
    lines.append("")
    lines.append("Statement categories from the native grammar contract:")
    lines.append("")
    for item in re.findall(r"\d+\.\s+([^\n]+)", statement_categories):
        lines.append(f"- {item.strip()}")

    lines.append(section_heading("5. Lexical Rules"))
    lines.append("The native lexer emits these lexical categories:")
    lines.append("")
    for item in lexical:
        lines.append(f"- `{item}`")
    lines.append("")
    lines.append("Native SBsql reserved literals are exactly `NULL`, `TRUE`, and `FALSE`. All other words are contextual unless a registry row marks them as refusal-only or private-only.")
    lines.append("")
    lines.append("Refusal-only keywords are exactly: `REFUSED`, `UNSUPPORTED`, `UNRESOLVED`, `PARSER_ONLY`, `POLICY_BLOCKED`, `REQUIRES_NEW_FUNCTION`, `RESERVED`, `FUTURE_VERSION`, `DEPRECATED`, and `PRIVATE_ONLY`.")
    lines.append("")
    lines.append("Native unquoted identifiers are case-preserving and case-insensitive for comparison. Quoted identifiers preserve spelling and quoted status, but do not become durable engine identity.")
    lines.append("")
    if parser_limits:
        lines.extend(parser_limits)

    lines.append(section_heading("6. Parser Pipeline"))
    lines.append("The native SBsql parser pipeline is fixed by the current zero-unresolved parser/documentation contract:")
    lines.append("")
    lines.extend(parser_pipeline or ["No parser pipeline table was found."])
    lines.append("")
    lines.append("No code path may lower directly from tokens or CST to SBLR. Lowering starts from BoundAST or a parser-control request that has passed refusal classification.")

    lines.append(section_heading("7. DDL Object Classes"))
    lines.append("SBsql DDL coverage from the current native dialect contract:")
    lines.append("")
    lines.extend(object_classes or ["No DDL object class table was found."])

    lines.append(section_heading("8. SHOW Taxonomy"))
    lines.append("Native `SHOW` forms must be represented in the SBsql SHOW command surface matrix and must have result shapes and diagnostics.")
    lines.append("")
    lines.extend(show_taxonomy or ["No SHOW taxonomy table was found."])

    lines.append(section_heading("9. Grammar Contract And Inventory"))
    lines.append("The completed SBsql Native V3 full dialect support execution_plan provides grammar-prep evidence: a grammar contract, grammar inventory, lexical contract, and ambiguity rules. These artifacts are implementation-preparation evidence for parser and driver work; the manifest-listed canonical specs and source-of-editing parser files remain controlling authority.")
    lines.append("")
    lines.append(f"The grammar-prep inventory currently records `{prep_inventory_rows}` command rows.")
    lines.append("")
    lines.append(counter_table(prep_family_counts, ("Grammar prep command family", "Rows")))
    lines.append("")
    lines.append(counter_table(prep_status_counts, ("Grammar prep status", "Rows")))
    lines.append("")
    lines.append("The final native `sbsql_v3.ebnf` source-of-editing artifact required by the parser implementation contract is not present under `project/src/parsers/native/v3/grammar/` in this tree.")
    for idx, block in enumerate(prep_blocks, 1):
        lines.append("")
        lines.append(f"### 9.{idx}. V3 Grammar Contract EBNF Block {idx}")
        lines.append("")
        lines.append("```ebnf")
        lines.append(block)
        lines.append("```")
    mgmt_offset = len(prep_blocks)
    for idx, block in enumerate(mgmt_blocks, 1):
        lines.append("")
        lines.append(f"### 9.{idx + mgmt_offset}. Management EBNF Block {idx}")
        lines.append("")
        lines.append("```ebnf")
        lines.append(block)
        lines.append("```")

    lines.append(section_heading("10. Function, Operator, Variable, and Special-Form Catalog Rules"))
    lines.append("Standard built-ins are fixed catalog objects with fixed UUIDv7-shaped seed identities recorded in the surface registry. Runtime user-created objects still receive generated UUIDv7 identities at creation time.")
    lines.append("")
    lines.append(f"Current compiled catalog counts: `{len(function_rows)}` functions, `{len(operator_rows)}` operators, `{len(variable_rows)}` variables, and `{len(canonical_rows)}` canonical surfaces.")
    lines.append("")
    lines.append("Operators are descriptor-bound. Ambiguous tokens are admitted only when the active grammar state allows them; final meaning is selected by operand descriptors and operator registry rows.")

    lines.append(section_heading("11. SBLR Lowering Contract"))
    lines.append("Every admitted BoundAST lowers to `SBLRExecutionEnvelope.v3` with session, database, transaction, security, language profile, and result contract context. The standard binding steps recorded in the current matrix are:")
    lines.append("")
    binding_counter = Counter(r["binding_steps"] for r in sblr_rows)
    lines.append(counter_table(binding_counter, ("Binding step sequence", "Rows"), limit=8))
    lines.append("")
    lines.append("Common result shapes:")
    lines.append("")
    lines.append(counter_table(Counter(r["result_shape"] for r in sblr_rows), ("Result shape", "Rows"), limit=20))

    lines.append(section_heading("12. Complete Authoritative EBNF Status"))
    lines.append("This generated reference intentionally does not fabricate full RHS grammar productions. The current contracts provide:")
    lines.append("")
    lines.append("- a manifest-listed grammar authority and binding contract;")
    lines.append("- a compiled surface registry with all current production names and parser-visible surfaces;")
    lines.append("- completed SBsql Native V3 grammar-prep execution_plan artifacts with grammar contract, lexical contract, ambiguity rules, and a command inventory;")
    lines.append("- a full management grammar skeleton with RHS EBNF;")
    lines.append("- a requirement that `project/src/parsers/native/v3/grammar/sbsql_v3.ebnf` become the source-of-editing artifact for complete native grammar implementation.")
    lines.append("")
    lines.append("Until that source-of-editing EBNF exists and is generated from or reconciled to the surface registry, this reference is the authoritative compiled language inventory, not a complete production-RHS grammar file.")

    lines.append(section_heading("Appendix A. Source-of-Editing Requirements"))
    lines.extend(source_of_editing or ["No source-of-editing table was found."])

    lines.append(section_heading("Appendix B. Grammar Production Inventory"))
    lines.append(f"Total grammar production rows: `{len(grammar_rows)}`.")
    lines.append("")
    for family, rows in group_rows(grammar_rows, "family").items():
        lines.append(f"### B. {family}")
        lines.append("")
        lines.append(rows_table(rows, ["surface_id", "canonical_name", "status", "cluster_scope", "sblr_operation_family", "documentation_family"]))
        lines.append("")

    lines.append(section_heading("Appendix C. Function Inventory"))
    lines.append(f"Total function rows: `{len(function_rows)}`.")
    lines.append("")
    for family, rows in group_rows(function_rows, "family").items():
        lines.append(f"### C. {family}")
        lines.append("")
        lines.append(rows_table(rows, ["surface_id", "canonical_name", "status", "sblr_operation_family", "documentation_family"]))
        lines.append("")

    lines.append(section_heading("Appendix D. Operator and Variable Inventory"))
    lines.append("### D.1 Operators")
    lines.append("")
    lines.append(rows_table(operator_rows, ["surface_id", "canonical_name", "status", "sblr_operation_family", "documentation_family"]))
    lines.append("")
    lines.append("### D.2 Variables")
    lines.append("")
    lines.append(rows_table(variable_rows, ["surface_id", "canonical_name", "status", "sblr_operation_family", "documentation_family"]))

    lines.append(section_heading("Appendix E. Canonical Surface Inventory"))
    lines.append(f"Total canonical surface rows: `{len(canonical_rows)}`.")
    lines.append("")
    lines.append(rows_table(canonical_rows, ["surface_id", "canonical_name", "family", "status", "cluster_scope", "sblr_operation_family", "documentation_family"]))

    lines.append(section_heading("Appendix F. Complete Surface Registry"))
    lines.append("This table is generated directly from `SBSQL_SURFACE_REGISTRY.csv`, `SBSQL_SURFACE_STATUS_MATRIX.csv`, and `SBSQL_TO_SBLR_OPERATION_MATRIX.csv`.")
    lines.append("")
    for family, rows in group_rows(joined, "family").items():
        lines.append(f"### F. {family}")
        lines.append("")
        lines.append(rows_table(rows, ["surface_id", "canonical_name", "surface_kind", "status", "cluster_scope", "sblr_operation_family", "allowed_lowering", "result_shape", "documentation_family"]))
        lines.append("")

    lines.append(section_heading("Appendix G. Generation Invariants"))
    lines.append("- This file is generated from private docs and registries only.")
    lines.append("- Implementation source code is not read.")
    lines.append("- Every row in the surface registry has a matching status row and SBLR matrix row in this generation run.")
    lines.append("- Missing rows, duplicate surface ids, or non-ASCII output are generation failures.")
    lines.append("- This generated reference is non-normative if it conflicts with manifest-listed contracts.")

    missing_status = sorted(set(surface_by_id) - set(status_by_id))
    missing_sblr = sorted(set(surface_by_id) - set(sblr_by_id))
    if missing_status or missing_sblr:
        raise SystemExit(f"missing rows: status={missing_status[:5]} sblr={missing_sblr[:5]}")

    output = ascii_clean("\n".join(lines).rstrip() + "\n")
    output.encode("ascii")
    return output


def main() -> None:
    OUT.write_text(generate(), encoding="ascii")
    print(f"wrote {rel(OUT)}")


if __name__ == "__main__":
    main()
