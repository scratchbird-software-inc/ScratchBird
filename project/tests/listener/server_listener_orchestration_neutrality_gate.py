#!/usr/bin/env python3
"""Guard server orchestration against parser-family and endpoint inference."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    print(f"server_listener_orchestration_neutrality_gate=failed:{message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    args = parser.parse_args()

    root = args.project_root
    paths = [
        root / "src/server/config.hpp",
        root / "src/server/config.cpp",
        root / "src/server/listener_orchestrator.hpp",
        root / "src/server/listener_orchestrator.cpp",
        root / "config/templates/SBsrv.conf",
    ]
    text = "\n".join(path.read_text(encoding="utf-8") for path in paths)

    forbidden = {
        "legacy_native_profile": "server.listener.native",
        "compiled_native_parser": 'SiblingExecutable("sbp_native")',
        "compiled_native_protocol": '"--protocol-family=native"',
        "compiled_native_profile": '"--listener-profile=native"',
        "compiled_native_package": '"--parser-package=sbp_native"',
        "compiled_native_dialect": '"--dialect-profile-uuid=sbwp-v1"',
        "compiled_default_port": "listener_native_port = 3050",
        "parser_binary_resolver": "parserBinaryForProtocol",
        "parser_executable_resolver": "ParserExecutablePath(",
    }
    for rule, token in forbidden.items():
        if token in text:
            fail(f"{rule}:{token}")

    family_literal = re.compile(
        r'"(?:firebird|postgres(?:ql)?|mysql|mariadb|sqlite|sbsql|native)"',
        re.IGNORECASE,
    )
    if family_literal.search(text):
        fail("compiled_parser_family_literal")

    required = (
        "std::vector<ServerListenerProfileConfig> listener_profiles",
        "for (const auto& configured : config.listener_profiles)",
        'SiblingExecutable("SBgate")',
        '"--protocol-family=" + profile->protocol_family',
        '"--parser-package=" + profile->parser_package_ref',
        '"--parser-executable=" + profile->parser_executable_path',
        '"--database-selector=" + profile->database_selector',
        '"--server-endpoint=" + profile->engine_endpoint',
        '"--bind-address=" + profile->bind_address',
        '"--port=" + std::to_string(profile->port)',
        "[server.listener]",
    )
    for token in required:
        if token not in text:
            fail(f"missing_generic_orchestration_contract:{token}")

    print("server_listener_orchestration_neutrality_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
