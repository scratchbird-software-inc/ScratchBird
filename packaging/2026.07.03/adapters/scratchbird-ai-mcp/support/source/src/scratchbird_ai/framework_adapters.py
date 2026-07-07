# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Thin framework adapters over the canonical ScratchBird AI service surface."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .deterministic import deterministic_id
from .service import ScratchBirdAIService


_FRAMEWORK_TOOL_NAMES = (
    "get_capabilities",
    "list_dialects",
    "list_schemas",
    "list_tables",
    "describe_table",
    "execute_readonly_query",
    "execute_mutation",
    "explain_query",
    "vector_search",
    "hybrid_search",
    "run_mutation",
)


@dataclass(slots=True, frozen=True)
class FrameworkToolDescriptor:
    name: str
    description: str
    canonical_tool_name: str
    interface_profile_id: str


@dataclass(slots=True, frozen=True)
class SemanticKernelFunctionDescriptor:
    plugin_name: str
    function_name: str
    canonical_tool_name: str
    description: str
    interface_profile_id: str


class _BaseFrameworkAdapter:
    interface_profile_id = "framework_adapter_v0"

    def __init__(self, service: ScratchBirdAIService) -> None:
        self.service = service

    def get_toolkit(self) -> tuple[FrameworkToolDescriptor, ...]:
        catalog = self.service.get_tool_descriptors()["tools"]
        descriptors: list[FrameworkToolDescriptor] = []
        for item in catalog:
            tool_name = str(item.get("tool_name", "")).strip()
            if tool_name not in _FRAMEWORK_TOOL_NAMES:
                continue
            descriptors.append(
                FrameworkToolDescriptor(
                    name=tool_name,
                    description=str(item.get("description", "")).strip(),
                    canonical_tool_name=tool_name,
                    interface_profile_id=self.interface_profile_id,
                )
            )
        return tuple(descriptors)

    def invoke_tool(
        self,
        *,
        tool_name: str,
        arguments: dict[str, Any] | None = None,
        security_context: dict[str, Any] | None = None,
        mode: str | None = None,
        approval_evidence: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        payload = self._build_payload(
            tool_name=tool_name,
            arguments=arguments,
            security_context=security_context,
            mode=mode,
            approval_evidence=approval_evidence,
            request_id=request_id,
        )
        return self.service.invoke_tool(
            payload=payload,
            interface_profile_id=self.interface_profile_id,
        )

    def _build_payload(
        self,
        *,
        tool_name: str,
        arguments: dict[str, Any] | None,
        security_context: dict[str, Any] | None,
        mode: str | None,
        approval_evidence: dict[str, Any] | None,
        request_id: str | None,
    ) -> dict[str, Any]:
        args = dict(arguments or {})
        normalized_request_id = (request_id or "").strip() or deterministic_id(
            "req",
            {
                "interface_profile_id": self.interface_profile_id,
                "tool_name": tool_name,
                "arguments": args,
            },
        )
        if security_context is not None:
            if tool_name in {
                "execute_readonly_query",
                "execute_mutation",
                "explain_query",
                "vector_search",
                "hybrid_search",
            }:
                args.setdefault("security_context", security_context)
            else:
                context = args.get("context", {})
                context_map = dict(context) if isinstance(context, dict) else {}
                context_map.setdefault("security_context", security_context)
                args["context"] = context_map
        if approval_evidence is not None:
            if tool_name == "execute_mutation":
                args.setdefault("approval_evidence", approval_evidence)
            elif tool_name == "run_mutation":
                args.setdefault(
                    "approval_token",
                    str(approval_evidence.get("approval_token", "")),
                )

        payload: dict[str, Any] = {
            "request_id": normalized_request_id,
            "call_id": deterministic_id(
                "call",
                {
                    "interface_profile_id": self.interface_profile_id,
                    "tool_name": tool_name,
                    "request_id": normalized_request_id,
                },
            ),
            "tool_name": tool_name,
            "arguments": args,
        }
        if security_context is not None:
            payload["security_context"] = security_context
        if mode is not None:
            payload["mode"] = mode
        if approval_evidence is not None:
            payload["approval_evidence"] = approval_evidence
        return payload


class LangChainAdapter(_BaseFrameworkAdapter):
    interface_profile_id = "langchain_v0"

    def run_query(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        options: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="execute_readonly_query",
            arguments={
                "dialect": dialect,
                "query_text": query_text,
                "options": options or {},
            },
            security_context=security_context,
            request_id=request_id,
        )

    def run_mutation(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        approval_evidence: dict[str, Any] | None = None,
        options: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="run_mutation",
            arguments={
                "dialect": dialect,
                "query_text": query_text,
                "approval_token": str((approval_evidence or {}).get("approval_token", "")),
                "options": options or {},
            },
            security_context=security_context,
            mode="ai_mutation_approved",
            approval_evidence=approval_evidence,
            request_id=request_id,
        )

    def explain(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="explain_query",
            arguments={"dialect": dialect, "query_text": query_text},
            security_context=security_context,
            request_id=request_id,
        )


class LlamaIndexAdapter(_BaseFrameworkAdapter):
    interface_profile_id = "llamaindex_v0"

    def query(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        options: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="execute_readonly_query",
            arguments={
                "dialect": dialect,
                "query_text": query_text,
                "options": options or {},
            },
            security_context=security_context,
            request_id=request_id,
        )

    def explain(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="explain_query",
            arguments={"dialect": dialect, "query_text": query_text},
            security_context=security_context,
            request_id=request_id,
        )

    def vector_retrieve(
        self,
        *,
        index_id: str,
        query_embedding: list[float],
        top_k: int,
        security_context: dict[str, Any],
        filters: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="vector_search",
            arguments={
                "index_id": index_id,
                "query_embedding": query_embedding,
                "top_k": top_k,
                "filters": filters or {},
            },
            security_context=security_context,
            request_id=request_id,
        )

    def hybrid_retrieve(
        self,
        *,
        dialect: str,
        query_text: str,
        query_embedding: list[float],
        vector_index_id: str,
        top_k: int,
        security_context: dict[str, Any],
        sql_filter: dict[str, Any] | None = None,
        weights: dict[str, Any] | None = None,
        options: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        return self.invoke_tool(
            tool_name="hybrid_search",
            arguments={
                "dialect": dialect,
                "query_text": query_text,
                "query_embedding": query_embedding,
                "vector_index_id": vector_index_id,
                "top_k": top_k,
                "sql_filter": sql_filter or {},
                "weights": weights or {},
                "options": options or {},
            },
            security_context=security_context,
            request_id=request_id,
        )


class SemanticKernelAdapter(_BaseFrameworkAdapter):
    interface_profile_id = "semantic_kernel_v0"
    plugin_name = "scratchbird"

    def get_plugin_functions(self) -> tuple[SemanticKernelFunctionDescriptor, ...]:
        functions: list[SemanticKernelFunctionDescriptor] = []
        for descriptor in self.get_toolkit():
            functions.append(
                SemanticKernelFunctionDescriptor(
                    plugin_name=self.plugin_name,
                    function_name=descriptor.name,
                    canonical_tool_name=descriptor.canonical_tool_name,
                    description=descriptor.description,
                    interface_profile_id=self.interface_profile_id,
                )
            )
        return tuple(functions)

    def invoke_function(
        self,
        *,
        function_name: str,
        arguments: dict[str, Any] | None = None,
        security_context: dict[str, Any] | None = None,
        mode: str | None = None,
        approval_evidence: dict[str, Any] | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        valid_names = {item.function_name for item in self.get_plugin_functions()}
        if function_name not in valid_names:
            return self.invoke_tool(
                tool_name=function_name,
                arguments=arguments or {},
                security_context=security_context,
                mode=mode,
                approval_evidence=approval_evidence,
                request_id=request_id,
            )
        return self.invoke_tool(
            tool_name=function_name,
            arguments=arguments or {},
            security_context=security_context,
            mode=mode,
            approval_evidence=approval_evidence,
            request_id=request_id,
        )
