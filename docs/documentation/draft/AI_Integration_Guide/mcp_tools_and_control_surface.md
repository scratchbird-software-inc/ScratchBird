# MCP Tools and Control Surface

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter lists the MCP tools exposed by the ScratchBird AI layer and
describes the native control surface families. All tools are registered through
`scratchbird_ai.mcp_server` using the `FastMCP` server scaffold under the
server name `scratchbird-ai`.

---

## How Tools Are Invoked

Tools are registered with the MCP runtime at startup. An AI client connects to
the MCP server and issues tool calls by name. Each tool call goes through an
error envelope path: if the underlying service raises an exception, the tool
returns a structured error object rather than propagating a raw exception.

The MCP server is installed by including the `mcp` optional dependency:

```
pip install -e ".[mcp]"
```

Starting the server:

```bash
# POSIX
PYTHONPATH=src tools/run_local_stack.sh

# Direct
python3 -m scratchbird_ai.mcp_server

# Windows console script (if installed)
scratchbird-ai-mcp
```

---

## Tool Inventory by Family

### Discovery and Compatibility

| Tool | Purpose |
| --- | --- |
| `get_capabilities` | Return the current capability matrix and supported dialect list |
| `get_tool_descriptors` | Return canonical tool declarations for all registered tools |
| `get_provider_profiles` | Return the direct provider tool-calling compatibility profiles |
| `get_compatibility_manifest` | Return the current compatibility manifest including version pins |
| `export_certification_manifest` | Export the release certification manifest |
| `negotiate_compatibility` | Fail-closed compatibility negotiation against a declared server/parser/driver context |
| `list_dialects` | List currently enabled dialects |

### Schema and Metadata Introspection

| Tool | Purpose |
| --- | --- |
| `list_schemas` | List schemas for a dialect and optional database |
| `list_tables` | List tables in a schema |
| `describe_table` | Describe a table's columns and types |

### Query Compilation and Execution

| Tool | Purpose |
| --- | --- |
| `compile_query` | Compile query text for a dialect; returns a `compile_artifact_id` |
| `execute_compiled` | Execute a previously compiled artifact by ID |
| `execute_readonly_query` | Compile and execute a read-only query in one call; requires `security_context` |
| `execute_mutation` | Compile and execute a mutation; requires `security_context` and `approval_evidence` |
| `run_query` | Convenience combine of compile + execute; accepts an `approval_token` for mutations |
| `run_mutation` | Mutation convenience wrapper; requires an `approval_token` |
| `explain_query` | Return the execution plan for a query without running it |

### Vector and Hybrid Retrieval

| Tool | Purpose |
| --- | --- |
| `create_vector_index` | Create a new vector index |
| `list_vector_indexes` | List vector indexes |
| `describe_vector_index` | Describe a vector index |
| `add_embeddings` | Add pre-computed embeddings to an index |
| `add_generated_embeddings` | Add embeddings that the tool generates from a provider config |
| `delete_embeddings` | Delete embeddings by vector ID |
| `reindex_vector_index` | Reindex a vector index |
| `delete_vector_index` | Delete a vector index |
| `vector_search` | K-nearest-neighbor vector search |
| `hybrid_search` | Combined vector + SQL hybrid search with configurable weights |

### Audit and Governance

| Tool | Purpose |
| --- | --- |
| `replay_audit_bundle` | Replay an audit bundle and verify tamper or policy mismatch |
| `list_audit_bundles` | List stored audit bundles |
| `validate_approval_evidence` | Validate an approval evidence object against the ledger |
| `list_approval_records` | List approval records from the durable ledger |
| `revoke_approval_record` | Administratively revoke an approval record |
| `create_audit_attestation` | Create an audit attestation (HMAC or external reference) |
| `verify_audit_attestation` | Verify a previously created attestation |

### Runtime Diagnostics and Operations

| Tool | Purpose |
| --- | --- |
| `get_runtime_diagnostics` | Return event summary counts, error rates, latency, and recent failures |
| `generate_operator_runbook_bundle` | Write a runbook bundle to the operator bundle output directory |

### Remote MCP Session Lifecycle

| Tool | Purpose |
| --- | --- |
| `open_remote_session` | Open a remote MCP session with auth negotiation |
| `invoke_remote_tool` | Invoke a tool within an established remote session |
| `close_remote_session` | Close a remote session |
| `poll_remote_operation` | Poll for the result of an async/streaming remote operation |
| `cancel_remote_operation` | Cancel an in-progress remote operation |

### Registry and Routing

| Tool | Purpose |
| --- | --- |
| `get_server_registry` | Return the current server registry |
| `register_remote_server` | Register a remote server in the registry |
| `update_remote_server_lifecycle` | Change the lifecycle state of a registered server |
| `report_remote_server_health` | Submit a health report for a registered server |
| `resolve_gateway_route` | Resolve the gateway route for a tool name and interface profile |

---

## Native Control Surface Families

The ScratchBird AI layer publishes a bounded, machine-readable native control
surface packet (`scratchbird_core_surface.py`). The packet is versioned at
`2026-04-18` and describes the following families:

### Graph Operations

Graph operation capability is declared in the `native` dialect capability
matrix (`graph_ops: true`). Graph tools operate over the native ScratchBird
execution path.

### Remote MCP

The remote MCP family covers the session lifecycle tools
(`open_remote_session`, `invoke_remote_tool`, `close_remote_session`,
`poll_remote_operation`, `cancel_remote_operation`). The default interface
profile is `mcp_remote_v0`. For details on authentication families, see
[remote_mcp_and_authentication.md](./remote_mcp_and_authentication.md).

### Registry and Routing Controls

The registry/routing family covers server registration, lifecycle management,
health reporting, and gateway route resolution. These tools allow AI
orchestration layers to track and route to multiple ScratchBird AI service
instances.

### Bridge and Runtime Management

The bridge and runtime management family covers the adapter mode, compatibility
negotiation, certification manifest export, and runtime diagnostics. These are
the tools an operator uses to inspect and manage the state of the AI layer
itself.

---

## Engine-Owned Retrieval Families

The ScratchBird core surface packet declares three engine-owned retrieval
families:

| Family ID | Semantic contract | Support state |
| --- | --- | --- |
| `vector_distance` | Vector similarity retrieval | implemented |
| `ann_hnsw` | k-NN/ANN retrieval | implemented |
| `full_text_inverted` | Full-text retrieval | implemented |

Retrieval metadata discovery is available through the `opensearch_meta.*`
catalog namespace with these relations:

| Relation | Support state |
| --- | --- |
| `opensearch_meta.index_metadata` | implemented |
| `opensearch_meta.mapping_fields` | implemented |
| `opensearch_meta.analyzer_settings` | implemented |
| `opensearch_meta.knn_index_metadata` | implemented |
| `opensearch_meta.aliases` | implemented |

---

## Framework Adapter Compatibility

The service layer also supports framework adapter wrappers. These are not MCP
tools but allow AI frameworks to interact with the service through familiar
interfaces:

| Framework | Profile ID | Status |
| --- | --- | --- |
| LangChain | `langchain_v0` | Supported |
| LlamaIndex | `llamaindex_v0` | Supported |
| Semantic Kernel | `semantic_kernel_v0` | Supported |

Provider tool-calling normalization is available through the
`provider_tool_calling_v0` profile, which normalizes OpenAI-style,
Anthropic-style, and Gemini-style provider payloads to the same canonical
execution results.
