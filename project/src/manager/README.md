# Manager

Manager lifecycle, cluster discovery, admission, join/renewal, proxy-gate behavior, manager protocol, and management operations.

Enterprise `sbmn_manager` profiles require command-scoped `manager.auth.mcp_secret_rights` for file/env MCP secrets. Wildcard MCP secret rights are developer-profile fixtures only.
`manager.validate_config` and `manager.reload_config` only accept `config_ref`
when it resolves to the manager's configured config file, and command responses
redact accepted config references.
Manager command payloads reject duplicate fields, unknown fields, and
operation-specific invalid values before dispatch.
Mutating manager operations require bounded token-shaped idempotency keys;
malformed replay keys fail closed before side effects.
