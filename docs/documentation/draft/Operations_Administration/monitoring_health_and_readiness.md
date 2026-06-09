# Monitoring, Health, And Readiness

## Purpose

This chapter defines the operational checks administrators use to decide whether ScratchBird components are alive, ready, healthy, degraded, draining, or refusing work.

## Initial Coverage

- liveness checks;
- readiness checks;
- health checks;
- startup and route readiness;
- database open readiness;
- parser registration readiness;
- transaction and cleanup summaries;
- storage health summaries;
- diagnostic counters;
- metrics scope;
- refusal states that should alert operators.

## Check Categories

| Check | Meaning |
| --- | --- |
| Liveness | The component is running and can answer a minimal check. |
| Readiness | The component is ready to accept intended work. |
| Health | The component and its required dependencies are in an acceptable operating state. |
| Drain | The component is intentionally refusing new work while existing work exits. |

## Related Pages

- [Service Lifecycle](service_lifecycle.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Getting Started: Diagnostics And Support Bundles](../Getting_Started/administration/diagnostics_and_support_bundles.md)
