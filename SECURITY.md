# Security Policy

## Supported Versions

This repository is an early beta public source-review release. Security fixes are handled for the current public beta branch unless a release note defines a narrower support window.

## Reporting

Do not report security-sensitive issues in public issues.

Send reports privately to the project maintainers using the private reporting channel supplied with the evaluation or investor-review package.

Include:

- affected commit or release tag;
- platform and build profile;
- reproduction steps;
- expected and observed behavior;
- relevant logs with credentials, private keys, tokens, and production data removed.

## Scope

In scope:

- authentication and authorization bypasses;
- unsafe default configuration;
- credential or secret exposure;
- memory safety issues;
- crash or corruption paths reachable through public interfaces;
- release packaging or artifact integrity problems.

Out of scope:

- unsupported cluster-provider behavior not present in this public source tree;
- third-party tool issues that must be reported upstream;
- benchmark performance claims;
- issues requiring private production data or credentials to reproduce.
