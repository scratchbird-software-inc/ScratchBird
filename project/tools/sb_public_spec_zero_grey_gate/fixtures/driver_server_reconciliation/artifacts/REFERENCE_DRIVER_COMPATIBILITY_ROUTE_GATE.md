# Reference Driver Compatibility Route Gate

Search key: `DRIVER_SERVER_REFERENCE_COMPATIBILITY_ROUTE`.

## Purpose

Make reference compatibility driver evidence prove the full ScratchBird route rather
than only proving parser acceptance or SQL text translation.

## Required Evidence

- Reference family and tool or compatibility driver used.
- ScratchBird parser/profile selected by the listener or route config.
- Authentication policy and user identity.
- Feature subset or compatibility profile under test.
- Original reference expected behavior source.
- ScratchBird route command.
- Result comparison rule.
- Refusal rule for features intentionally outside the profile.
- CTest label and artifact location.

## Closure Rule

`DSR-045` closes only when reference compatibility lanes run through the admitted
client or tool to wire or IPC to listener/parser/server/engine route and record
pass/fail evidence.
