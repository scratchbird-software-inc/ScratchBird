# Operating Modes Runbook

## Purpose

This chapter turns the high-level operating modes into operator runbook sections. It should tell an administrator what must be configured, started, verified, stopped, and diagnosed for each mode.

## Initial Coverage

- embedded engine runbook;
- single-node IPC server runbook;
- standalone listener and parser route runbook;
- managed group deployment runbook;
- mode-specific smoke tests;
- attach and detach checks;
- start, stop, drain, and restart checks;
- diagnostics to collect for each mode;
- boundaries that each mode does not imply.

## Runbook Shape

Each mode should eventually include:

1. prerequisites;
2. configuration inputs;
3. startup sequence;
4. readiness checks;
5. first transaction proof;
6. failure and refusal checks;
7. clean shutdown;
8. restart and reopen proof.

## Related Pages

- [Service Lifecycle](service_lifecycle.md)
- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Getting Started: Choosing A Mode Summary](../Getting_Started/operating_modes/choosing_a_mode_summary.md)
