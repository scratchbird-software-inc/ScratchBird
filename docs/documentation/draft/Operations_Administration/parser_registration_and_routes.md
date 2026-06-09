# Parser Registration And Routes

## Purpose

This chapter defines how parser packages are registered, selected, routed, diagnosed, and refused when unavailable or out of scope.

## Initial Coverage

- parser package discovery;
- parser version and ABI checks;
- route names and endpoint binding;
- native SBsql parser route;
- compatibility parser routes;
- parser-visible schema roots and workareas;
- parser-specific defaults;
- parser package isolation;
- unsupported surface refusal;
- diagnostics when a parser is missing, incompatible, or denied.

## Route Principles

- Each parser is a standalone capability.
- One parser should not silently accept another parser's language.
- Parser acceptance does not bypass engine authorization.
- Parser route configuration should be explicit and testable.

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Identity, Security, And Policy](identity_security_and_policy.md)
- [Getting Started: Engine Parser Boundary](../Getting_Started/architecture/engine_parser_boundary.md)
