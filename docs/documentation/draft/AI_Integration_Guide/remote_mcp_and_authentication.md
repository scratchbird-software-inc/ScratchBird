# Remote MCP and Authentication

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes the remote MCP session lifecycle and the authentication
families that the ScratchBird AI layer advertises for remote clients. It also
covers the transport modes available for remote sessions.

---

## Remote MCP Session Lifecycle

A remote MCP session is an authenticated, time-bounded connection between a
remote AI client and the ScratchBird AI service. The session lifecycle has four
phases:

1. **Open** — `open_remote_session` accepts an authentication envelope and
   negotiates the protocol version and transport. On success it returns a
   `session_id`, `session_expires_at`, and `heartbeat_interval_sec`.

2. **Use** — `invoke_remote_tool` submits tool calls within the session. Long-
   running or streaming operations return an `operation_id` that can be polled
   with `poll_remote_operation`. In-progress operations can be cancelled with
   `cancel_remote_operation`.

3. **Poll/Cancel** — `poll_remote_operation` accepts an optional
   `continuation_token` for paged streaming results. `cancel_remote_operation`
   accepts a `reason` and terminates the operation.

4. **Close** — `close_remote_session` terminates the session. Calling close on
   an already-closed session returns `status: already_closed` without error.

Sessions expire automatically at `session_expires_at` (default TTL: 900
seconds). The default heartbeat interval is 30 seconds. Expired sessions
return `E_SESSION_REQUIRED` with policy rule ID `REMOTE-SESSION-006`.

---

## Interface Profile

The only currently supported remote interface profile is `mcp_remote_v0`.
Requests for any other profile return:

```
E_INTERFACE_PROFILE_UNSUPPORTED / REMOTE-SESSION-001
```

The default is applied automatically when `interface_profile_id` is omitted.

---

## Protocol Version

The current supported remote protocol version is `v0`. Other values return:

```
E_COMPONENT_VERSION_UNSUPPORTED / REMOTE-SESSION-002
```

---

## Transport Modes

The following transport modes are advertised as supported for remote sessions:

| Transport | Description |
| --- | --- |
| `https_json_request_response` | Standard HTTPS request-response (default) |
| `https_sse_server_stream` | HTTPS with server-sent events for streaming responses |
| `websocket_bidirectional` | WebSocket bidirectional transport |

Requesting an unsupported transport returns:

```
E_TRANSPORT_PROFILE_UNSUPPORTED / REMOTE-SESSION-003
```

---

## Authentication Families

The `open_remote_session` call requires an `auth_envelope` object. The
`RemoteSessionManager` advertises the following `supported_auth_types`:

| Auth type | Description |
| --- | --- |
| `bearer` | Static bearer token. Validated against `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` when that variable is set. Inferred automatically if `token`, `access_token`, or `jwt` fields are present without an explicit `auth_type`. |
| `oauth2_access_token` | OAuth2 access token. Validated against the bridge token when set. |
| `jwt_bearer` | JWT bearer token. Validated against the bridge token when set. |
| `workload_identity` | Workload identity credential (e.g., cloud service account). Provide `workload_identity` field in the auth envelope. |
| `proxy_principal` | A principal forwarded by a trusted proxy. Provide `proxy_principal` or `principal` field. |
| `ldap_bind` | LDAP bind credential. Provide `bind_dn` field in the auth envelope. |
| `kerberos_gssapi` | Kerberos/GSSAPI credential. Provide `kerberos_principal` field. |
| `radius_pap` | RADIUS PAP credential. Provide `radius_username` field. |
| `pam_conversation` | PAM conversation credential. Provide `pam_service` field. |
| `preauthenticated_context` | The caller asserts pre-authentication; provide `security_context` in the envelope. Inferred automatically when no token fields are present. |

**Token validation note:** When `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` is
configured, auth types `bearer`, `oauth2_access_token`, and `jwt_bearer` are
validated against that token value. Other auth types are admitted based on the
presence of the expected envelope field, not against a server-side credential
store in the current early-beta surface.

**Auth material requirements:** Each auth type requires a specific envelope
field to be non-empty. If the required field is missing, the call returns
`E_POLICY_DENY / REMOTE-AUTH-004`. The `preauthenticated_context` type
requires a `security_context` object.

---

## Auth Envelope Format

The auth envelope is a JSON object passed as the `auth_envelope` argument to
`open_remote_session`. Minimal examples:

```json
{
  "auth_type": "bearer",
  "token": "<your-token>",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

```json
{
  "auth_type": "preauthenticated_context",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

```json
{
  "auth_type": "ldap_bind",
  "bind_dn": "cn=user,dc=example,dc=com",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

The `security_context` object within the envelope is used to populate the
session's security context for downstream policy and audit checks. It should
include at minimum `tenant_id` and `actor_id`.

---

## Security Context Propagation

The `security_context` derived from the auth envelope is carried through the
session and used for:

- Approval record matching (tenant and actor identity)
- Audit bundle subject fields
- Operational control enforcement (quota per tenant and actor)
- Registry and routing security checks

---

## Error Codes Reference

| Error code | Policy rule | Condition |
| --- | --- | --- |
| `E_INTERFACE_PROFILE_UNSUPPORTED` | `REMOTE-SESSION-001` | Unsupported interface profile requested |
| `E_COMPONENT_VERSION_UNSUPPORTED` | `REMOTE-SESSION-002` | Unsupported protocol version requested |
| `E_TRANSPORT_PROFILE_UNSUPPORTED` | `REMOTE-SESSION-003` | Unsupported transport requested |
| `E_TOOL_INPUT_INVALID` | `REMOTE-SESSION-004` | `client_capabilities` is not an object |
| `E_SESSION_REQUIRED` | `REMOTE-SESSION-005` | Unknown session ID |
| `E_SESSION_REQUIRED` | `REMOTE-SESSION-006` | Expired session ID |
| `E_POLICY_DENY` | `REMOTE-AUTH-001` | `auth_envelope` is not an object |
| `E_PROVIDER_CONTRACT_UNSUPPORTED` | `REMOTE-AUTH-002` | Unsupported auth type |
| `E_POLICY_DENY` | `REMOTE-AUTH-003` | Token validation failure (bearer/oauth2/jwt) |
| `E_POLICY_DENY` | `REMOTE-AUTH-004` | Missing auth material for declared auth type |
