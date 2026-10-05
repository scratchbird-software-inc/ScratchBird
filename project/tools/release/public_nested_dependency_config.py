# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Forward dependency locations, never cached qualification results, to nested builds."""

from __future__ import annotations

from pathlib import Path


DEPENDENCY_CACHE_KEYS = (
    "CMAKE_PREFIX_PATH",
    "CMAKE_LIBRARY_PATH",
    "CMAKE_INCLUDE_PATH",
    "SBL_NUMERIC_MPFR_INCLUDE_DIR",
    "SBL_NUMERIC_MPFR_LIBRARY",
    "SBL_NUMERIC_GMP_INCLUDE_DIR",
    "SBL_NUMERIC_GMP_LIBRARY",
    "SBL_NUMERIC_BOOST_INCLUDE_DIR",
)
CACHE_TYPES = frozenset({"PATH", "FILEPATH", "STRING", "UNINITIALIZED"})


def dependency_configure_args(cache_path: Path | None) -> list[str]:
    """Retain configured dependency hints as individual CMake argv elements.

    A standalone invocation can omit the parent cache and use normal CMake
    dependency discovery. An explicitly supplied cache must be readable and
    unambiguous. Never import compiler flags, build/test switches, output
    directories, or the reference backend's cached link/TLS success results.
    The nested project still performs its own mandatory backend checks.
    """
    if cache_path is None:
        return []
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        key, type_separator, cache_type = declaration.rpartition(":")
        if not type_separator:
            if declaration in DEPENDENCY_CACHE_KEYS:
                raise ValueError(f"missing nested dependency cache type: {declaration}")
            continue
        if key not in DEPENDENCY_CACHE_KEYS:
            continue
        if not separator:
            raise ValueError(f"missing nested dependency value: {key}")
        if key in values:
            raise ValueError(f"duplicate nested dependency setting: {key}")
        if cache_type not in CACHE_TYPES:
            raise ValueError(f"invalid nested dependency cache type: {key}")
        if "\0" in value or value == "NOTFOUND" or value.endswith("-NOTFOUND"):
            raise ValueError(f"unresolved nested dependency setting: {key}")
        if not value and key.startswith("SBL_NUMERIC_"):
            raise ValueError(f"empty nested dependency setting: {key}")
        # Values are not shell commands. Preserve spaces, equals signs,
        # backslashes and CMake list separators without shell re-parsing.
        values[key] = value
    return [f"-D{key}={values[key]}" for key in DEPENDENCY_CACHE_KEYS if values.get(key)]
