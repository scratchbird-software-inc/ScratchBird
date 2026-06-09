# Release Validation Checklist

## Purpose

This chapter defines an operator-facing checklist for validating a ScratchBird build before broader testing or release use.

## Initial Coverage

- output tree completeness;
- required resources present;
- license and notices present where required;
- parser package presence and route tests;
- configuration validation;
- embedded smoke test;
- IPC smoke test;
- listener and parser smoke test;
- managed group entry smoke test where configured;
- database create/open/reopen proof;
- transaction commit and rollback proof;
- backup and restore drill;
- diagnostics and support-bundle redaction proof;
- platform-specific test status;
- known limitations review.

## Checklist Rule

The checklist should be evidence-driven. The presence of a file, directory, or command name is not enough; the behavior must be run and the proof must be reviewable.

## Related Pages

- [Installation And Output Layout](installation_and_output_layout.md)
- [Operating Modes Runbook](operating_modes_runbook.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
