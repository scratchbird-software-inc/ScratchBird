# ScratchBird Operations And Administration Guide

This directory contains the draft ScratchBird Operations And Administration Guide. It is the operator-facing companion to the Getting Started Guide and the SBsql Language Reference.

The guide explains how to install, configure, run, diagnose, validate, and maintain ScratchBird deployments without treating diagrams or command names as release guarantees. Every operational claim must be checked against the current build output, target platform, configuration, tests, and release notes.

## Directory Map

| Chapter | Purpose |
| --- | --- |
| [Installation And Output Layout](installation_and_output_layout.md) | How staged binaries, parser packages, resource files, configuration, and proof assets should be organized for a usable build. |
| [Configuration Reference](configuration_reference.md) | Configuration areas administrators must understand before starting services or opening databases. |
| [Operating Modes Runbook](operating_modes_runbook.md) | Runbook-level guidance for embedded, IPC, standalone listener, and managed group deployments. |
| [Service Lifecycle](service_lifecycle.md) | Start, readiness, drain, stop, restart, stale endpoint handling, and failure response. |
| [Identity, Security, And Policy](identity_security_and_policy.md) | Authentication, authorization, schema roots, workareas, protected material, and redaction policy. |
| [Parser Registration And Routes](parser_registration_and_routes.md) | Parser package registration, route selection, compatibility boundaries, and refusal behavior. |
| [Database Lifecycle](database_lifecycle.md) | Create, open, close, reopen, detach, attach, verify, refuse, and recover database lifecycle concepts. |
| [Filespaces And Storage](filespaces_and_storage.md) | Filespace identity, storage placement, primary filespace behavior, and storage diagnostics. |
| [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md) | Logical backup/restore, import/export, migration, CDC, replication, ETL, and denied physical or low-level routes. |
| [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md) | Diagnostic classes, support-bundle content, redaction, and operator review. |
| [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md) | Health checks, readiness checks, liveness checks, metrics, and operational state. |
| [Troubleshooting](troubleshooting.md) | Symptom-oriented diagnosis paths for startup, connection, parser, security, storage, and transaction issues. |
| [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md) | Version policy, format compatibility, parser package compatibility, and unsupported downgrade refusal. |
| [Release Validation Checklist](release_validation_checklist.md) | Operator-facing checklist for validating a build before broader use. |

## Reading Model

Start with [Installation And Output Layout](installation_and_output_layout.md), then [Configuration Reference](configuration_reference.md), then the runbook chapter for the operating mode being tested.

Use the [Getting Started Guide](../Getting_Started/README.md) for conceptual orientation and the [Language Reference](../Language_Reference/README.md) for SBsql syntax and catalog details.

## Draft Status

This is a draft manual baseline. It establishes the chapter structure and expansion scope for the operations documentation.
