# ScratchBird Operations And Administration Guide

This guide is the operator-facing companion to the Getting Started Guide and the SBsql Language Reference. It covers everything you need to install, configure, run, diagnose, and maintain ScratchBird deployments — from the moment you unpack a build output to the moment you decommission a database.

ScratchBird has a layered architecture: a core engine library, an IPC server that hosts it, a listener that accepts client connections and routes them through a parser package, and an optional single-node manager that supervises all three. Each layer has its own configuration file, its own lifecycle states, and its own failure behavior. That layering is intentional — it means you can embed the engine directly in a trusted application, or expose it over a network via a full managed stack, or anywhere in between. This guide follows that progression.

Every operational claim in this guide has been verified against the source tree. Build-configuration-dependent behavior is hedged explicitly. Generic advice — back up regularly, test restores — needs no source citation.

## Directory Map

| Chapter | Purpose |
| --- | --- |
| [Installation And Output Layout](installation_and_output_layout.md) | What a staged build output contains, where each file lives, and how to verify that the pieces match. |
| [Configuration Reference](configuration_reference.md) | Every configuration file with its real sections, keys, and defaults, plus an explanation of how configuration is loaded. |
| [Operating Modes Runbook](operating_modes_runbook.md) | Step-by-step runbooks for embedded, IPC-server, standalone-listener, and managed-group deployments. |
| [Service Lifecycle](service_lifecycle.md) | Start, readiness, drain, stop, restart, stale endpoint handling, and failure response. |
| [Identity, Security, And Policy](identity_security_and_policy.md) | Authentication, authorization, schema roots, workareas, protected material, and redaction policy. |
| [Parser Registration And Routes](parser_registration_and_routes.md) | Parser package registration, route selection, compatibility boundaries, and refusal behavior. |
| [Database Lifecycle](database_lifecycle.md) | Create, open, close, reopen, detach, attach, verify, refuse, and recover database lifecycle concepts. |
| [Filespaces And Storage](filespaces_and_storage.md) | Filespace identity, storage placement, primary filespace behavior, and storage diagnostics. |
| [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md) | Logical backup and restore, import and export, migration, CDC, replication, ETL, and denied physical routes. |
| [External Git Catalog Versioning](external_git_catalog_versioning.md) | Opt-in export of the catalog as content-hashed artifacts for external Git versioning, diffing, and rollback planning — with engine authority preserved throughout. |
| [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md) | Diagnostic classes, support-bundle content, redaction, and operator review. |
| [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md) | Health checks, readiness checks, liveness checks, metrics, and operational state. |
| [Troubleshooting](troubleshooting.md) | Symptom-oriented diagnosis paths for startup, connection, parser, security, storage, and transaction issues. |
| [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md) | Version policy, format compatibility, parser package compatibility, and unsupported downgrade refusal. |
| [Release Validation Checklist](release_validation_checklist.md) | Operator-facing checklist for validating a build before broader use. |

## Reading Model

If you are setting up ScratchBird for the first time, start with [Installation And Output Layout](installation_and_output_layout.md), then read [Configuration Reference](configuration_reference.md), then open the runbook section in [Operating Modes Runbook](operating_modes_runbook.md) that matches the deployment you are building.

If something has gone wrong, jump to [Service Lifecycle](service_lifecycle.md) for state-machine context, then [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md) for health check commands, then [Troubleshooting](troubleshooting.md) for symptom-specific paths.

The [Getting Started Guide](../Getting_Started/README.md) is a better first read if you are new to ScratchBird's concepts. The [Language Reference](../Language_Reference/README.md) covers SBsql syntax, catalog details, and data types.

## Draft Status

This is a draft manual. Technical claims have been verified against the source tree and build outputs. Generic operational guidance is provided without source citation. Claims that could not be verified from source have been omitted.
