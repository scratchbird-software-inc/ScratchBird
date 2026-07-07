# Source

Implementation code for ScratchBird AI integration.

## Package Layout

- `scratchbird_ai/contracts.py` - request/response contracts
- `scratchbird_ai/capability_matrix.py` - matrix loading utilities
- `scratchbird_ai/compatibility.py` - machine-readable compatibility manifest and negotiation helpers
- `scratchbird_ai/environment_manifest.py` - certification/environment manifest export helpers
- `scratchbird_ai/framework_adapters.py` - LangChain, LlamaIndex, and Semantic Kernel compatibility layers over the canonical service
- `scratchbird_ai/interface_profiles.py` - canonical interface profile inventory for capability advertisement
- `scratchbird_ai/provider_profiles.py` - direct provider compatibility profile inventory
- `scratchbird_ai/remote_sessions.py` - remote MCP session establishment and auth binding
- `scratchbird_ai/operation_streams.py` - long-running operation, event, continuation, and cancellation support
- `scratchbird_ai/retrieval.py` - retrieval index lifecycle, persistence, and engine-free vector/hybrid search
- `scratchbird_ai/tool_schema.py` - canonical tool descriptors, argument validation, provider normalization, and structured-output validation
- `scratchbird_ai/router.py` - dialect routing + capability gating
- `scratchbird_ai/policy.py` - read-only and approval policy enforcement
- `scratchbird_ai/service.py` - compile/execute orchestration service
- `scratchbird_ai/mcp_server.py` - MCP tool server registration and entrypoint
- `scratchbird_ai/adapters/` - backend adapter interfaces and scaffolding adapters

## Local Usage

- Validate matrix: `./tools/validate_capability_matrix.py`
- Run tests: `PYTHONPATH=src python3 -m unittest discover -s tests -p "test_*.py"`
- Run MCP server (after installing optional deps):
  - `pip install .[mcp]`
  - `scratchbird-ai-mcp`

## Adapter Configuration

Environment variables:

- `SCRATCHBIRD_AI_ADAPTER_MODE` - `mock`, `http`, or `hybrid` (default: `mock`)
- `SCRATCHBIRD_AI_HTTP_BASE_URL` - HTTP service base URL for real adapters
- `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC` - adapter HTTP timeout in seconds
- `SCRATCHBIRD_AI_HTTP_API_TOKEN` - optional shared token for bridge/API access
- `SCRATCHBIRD_AI_HTTP_DIALECTS` - comma-separated dialects for HTTP mode in `hybrid` (default `native`)
- `SCRATCHBIRD_AI_RETRIEVAL_CATALOG_PATH` - optional JSON catalog path for persistent retrieval indexes
- `SCRATCHBIRD_AI_REMOTE_MCP_AUTH_TOKEN` - optional shared-token verifier for token-based remote MCP auth
- `SCRATCHBIRD_AI_REMOTE_MCP_SESSION_TTL_SEC` - remote session TTL in seconds
- `SCRATCHBIRD_AI_REMOTE_MCP_HEARTBEAT_INTERVAL_SEC` - remote session heartbeat interval in seconds
- `SCRATCHBIRD_AI_REMOTE_MCP_PROTOCOL_VERSIONS` - comma-separated supported remote protocol versions
- `SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_AUTH_TYPES` - comma-separated supported remote auth types
- `SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_TRANSPORTS` - comma-separated supported remote transports

Support scope:

- AI runtime support in this repository is ScratchBird-native only.
- ScratchBird-native control surfaces such as graph operations, remote MCP, and bridge/runtime management are advertised as supported; authorization is enforced by ScratchBird server policy, grants, and group membership.

See `docs/releases/EARLY_BETA_CONFORMANCE_GATES.md` and the HTTP bridge tests
for the public endpoint contract exercised by this release.
