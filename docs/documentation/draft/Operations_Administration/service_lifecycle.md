# Service Lifecycle

## Purpose

This chapter defines how ScratchBird services should be started, checked, drained, stopped, restarted, and diagnosed.

## Initial Coverage

- startup sequence;
- configuration validation before startup;
- database open and route activation;
- readiness and liveness checks;
- client attach and detach behavior;
- drain mode;
- clean shutdown;
- forced shutdown;
- restart behavior;
- stale endpoint and stale process handling;
- refusal behavior when lifecycle state is unsafe.

## Lifecycle States

The expanded chapter should distinguish states such as configured, starting, ready, draining, stopped, refused, recovery-required, and operator-action-required.

## Related Pages

- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Getting Started: Standalone Server](../Getting_Started/operating_modes/standalone_server.md)
