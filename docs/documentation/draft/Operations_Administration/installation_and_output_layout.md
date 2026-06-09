# Installation And Output Layout

## Purpose

This chapter defines what a staged ScratchBird output should contain before an administrator tries to run it. A usable output is more than one executable; it includes runtime binaries, parser packages, resource files, configuration, and validation material that belongs to the same build.

## Initial Coverage

- platform-specific output directories;
- runtime binaries and shared libraries;
- parser package placement;
- command-line tool placement;
- character set, collation, time zone, and policy resources;
- configuration templates;
- proof and smoke-test material;
- separation between source, build output, release output, and live databases;
- permissions and ownership expectations;
- cleanup rules for temporary or generated files.

## Operator Questions

- Which files are required for the selected mode?
- Which files are generated artifacts rather than source?
- Which files are safe to redistribute?
- Where should live databases be created?
- How does an operator verify that resources match the binaries?

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Release Validation Checklist](release_validation_checklist.md)
- [Getting Started: Configuration Basics](../Getting_Started/administration/configuration_basics.md)
