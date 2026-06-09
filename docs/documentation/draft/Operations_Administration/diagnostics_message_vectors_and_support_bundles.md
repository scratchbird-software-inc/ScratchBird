# Diagnostics, Message Vectors, And Support Bundles

## Purpose

This chapter defines the operator-facing diagnostic model: message vectors, log events, refusal classes, support-bundle contents, redaction, and review.

## Initial Coverage

- message-vector classes;
- parser diagnostics;
- engine diagnostics;
- startup and shutdown diagnostics;
- database open diagnostics;
- transaction diagnostics;
- storage diagnostics;
- security and policy diagnostics;
- support-bundle generation;
- redaction policy;
- operator review before sharing.

## Diagnostic Rule

Diagnostics should distinguish unsupported, denied, unavailable, unsafe, invalid, and recovery-required states. A controlled refusal is an expected operational outcome.

## Related Pages

- [Troubleshooting](troubleshooting.md)
- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Language Reference: Refusal Vectors](../Language_Reference/syntax_reference/refusal_vectors.md)
