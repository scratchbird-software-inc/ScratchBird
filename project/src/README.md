# Implementation Source

All implementation source is organized by contract-owned subsystem boundaries.

Forbidden here:

- parser code inside IPC;
- compatibility-specific engine behavior inside core engine modules;
- SQL text as engine execution authority;
- private cluster authority in public package paths;
- generated build output committed as source.
