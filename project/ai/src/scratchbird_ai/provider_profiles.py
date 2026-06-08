# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Provider-specific compatibility profile descriptors."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .interface_profiles import INTERFACE_COMPATIBILITY_VERSION


@dataclass(slots=True, frozen=True)
class ProviderProfileDescriptor:
    profile_id: str
    provider_name: str
    provider_runtime_family: str
    state: str
    tool_calling_mode: str
    structured_output_modes: tuple[str, ...]
    streaming_support: bool
    compatibility_version: str = INTERFACE_COMPATIBILITY_VERSION
    unsupported_features: tuple[str, ...] = ()
    evidence_gate: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "provider_name": self.provider_name,
            "provider_runtime_family": self.provider_runtime_family,
            "state": self.state,
            "tool_calling_mode": self.tool_calling_mode,
            "structured_output_modes": list(self.structured_output_modes),
            "streaming_support": self.streaming_support,
            "compatibility_version": self.compatibility_version,
            "unsupported_features": list(self.unsupported_features),
            "evidence_gate": self.evidence_gate,
        }


_PROVIDER_PROFILES = (
    ProviderProfileDescriptor(
        profile_id="openai_tool_calling_v0",
        provider_name="OpenAI-compatible",
        provider_runtime_family="openai_tool_calling",
        state="implemented",
        tool_calling_mode="single_function_call",
        structured_output_modes=("json_object", "json_schema"),
        streaming_support=False,
        unsupported_features=(
            "multi_tool_calls",
            "partial_result_streaming",
            "background_execution",
            "cancellation",
        ),
        evidence_gate="EVID-03",
    ),
    ProviderProfileDescriptor(
        profile_id="anthropic_tool_use_v0",
        provider_name="Anthropic-compatible",
        provider_runtime_family="anthropic_tool_use",
        state="implemented",
        tool_calling_mode="single_tool_use",
        structured_output_modes=("json_object",),
        streaming_support=False,
        unsupported_features=(
            "schema_bound_structured_output",
            "partial_result_streaming",
            "background_execution",
            "cancellation",
        ),
        evidence_gate="EVID-03",
    ),
    ProviderProfileDescriptor(
        profile_id="gemini_function_calling_v0",
        provider_name="Gemini-compatible",
        provider_runtime_family="gemini_function_calling",
        state="implemented",
        tool_calling_mode="single_function_call",
        structured_output_modes=("json_object",),
        streaming_support=False,
        unsupported_features=(
            "schema_bound_structured_output",
            "partial_result_streaming",
            "background_execution",
            "cancellation",
        ),
        evidence_gate="EVID-03",
    ),
)


def get_provider_profiles() -> list[dict[str, Any]]:
    return [profile.to_dict() for profile in _PROVIDER_PROFILES]


def get_provider_profile_descriptor(profile_id: str) -> ProviderProfileDescriptor:
    normalized = str(profile_id).strip()
    for profile in _PROVIDER_PROFILES:
        if profile.profile_id == normalized:
            return profile
    raise KeyError(normalized)
