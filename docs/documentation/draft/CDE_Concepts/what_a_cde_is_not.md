# What A CDE Is Not

## Purpose

The Convergent Data Engine label describes an architectural design. It carries
real meaning — one engine authority, many models, many dialects, autonomous
operation, layered acceleration — but it does not carry every meaning that the
phrase might suggest on first reading. This page states the non-goals, scope
limits, and explicit non-promises that bound the category. It is important to
read this alongside the positive description in
[what_is_a_convergent_data_engine.md](what_is_a_convergent_data_engine.md).

**This is a draft.** The limits described here are features of the design, not
temporary gaps to be filled. They are choices.

---

## Not A Universal Dialect Promise

ScratchBird accepts multiple client languages and wire protocols. This does not
mean it can accept any language or that every feature of any supported language
is available.

Each compatibility parser covers a scoped subset of its target dialect. Features
outside that scope are refused with a diagnostic. The scope of each compatibility
surface is defined by:

- What can be correctly and safely lowered to SBLR.
- What the engine actually supports for the given model family.
- What is declared in the current build's compatibility configuration.

There is no implicit commitment to achieving parity with any other engine or
interface that speaks a similar language. If a specific operation you depend on
is not in scope, the system will tell you — not silently substitute something else.

**How to verify:** The compatibility scope for each supported dialect is
documented in
[../Getting_Started/using_scratchbird/reference_system_compatibility.md](../Getting_Started/using_scratchbird/reference_system_compatibility.md)
and in the per-parser documentation under the Language Support guide. Before
building a compatibility reliance, confirm that the specific operations you need
are in scope for the current build.

---

## Not Automatic Or Zero-Configuration Compatibility

Routing a client connection through a compatibility dialect requires explicit
configuration: the parser package must be registered, the route must be
enabled for the target database, and the compatibility namespace must be
configured if needed. Compatibility is opt-in and per-database.

A compatibility dialect that is installed in the build but not configured for a
given database is not accessible to connections for that database. A
compatibility name path that is not registered in the catalog is not resolved.

This is not a limitation to work around — it is a deliberate design. You should
know exactly what compatibility surfaces are active for any database. Implicit
or automatic compatibility activation would make the system's behavior harder
to reason about.

**How to verify:** Check parser registration and route configuration via the
operations documentation at
[../Operations_Administration/parser_registration_and_routes.md](../Operations_Administration/parser_registration_and_routes.md).

---

## Not A Guarantee That Every Model Family Is Complete In Every Build

Build configuration controls which type families, provider families, parser
packages, and acceleration layers are compiled in. A feature is available only
when all of the following are true for the current deployment:

- The feature is compiled into the current build.
- The required library or hardware dependency is present.
- The feature is enabled in configuration for the current database.
- The feature has been validated in the release notes or test coverage for the
  current build target.

Do not assume that because a type family or model family appears in this
documentation it is fully operational in your specific build configuration.

**How to verify:** Use the engine's own capability and diagnostics surfaces to
confirm what is available. The release validation checklist at
[../Operations_Administration/release_validation_checklist.md](../Operations_Administration/release_validation_checklist.md)
provides a structured approach.

---

## No Silent Behavior Substitution

If ScratchBird cannot execute a request correctly within its correctness
guarantees — because the operation is out of scope for the compatibility surface,
because the required capability is absent, because the result would violate MGA
visibility or security policy — it refuses with a diagnostic.

It does not:

- Approximate a result that is close but not exactly correct.
- Silently execute a weaker or different version of the requested operation.
- Fall back to a different algorithm that changes the semantic meaning of the
  result.
- Return partial results without indicating that they are partial.

This is the no-silent-substitution policy. Diagnostics are how the system
communicates its scope. A diagnostic refusal is not a bug; it is the system
telling you clearly what it cannot do so you can make an informed decision
about how to proceed.

---

## Not A Production Or Certification Claim

The CDE category label is architectural and descriptive. It does not constitute:

- A production readiness certification.
- A security accreditation (FIPS, Common Criteria, SOC 2, or equivalent).
- A performance benchmark or throughput guarantee.
- A high availability or uptime commitment.
- A compatibility certification with any named product's test suite.

These assessments must be performed independently against a specific build,
configuration, and deployment context. This documentation provides the
architectural basis for such assessments; it does not substitute for them.

---

## Not A Substitute For Specialized Expertise

A CDE that handles many data models does not mean that deep expertise in each
model family is unnecessary. Correct use of graph traversal semantics, vector
index tuning, time-series compression, or full-text relevance scoring requires
understanding the domain. ScratchBird provides the engine; you must understand
the workload.

Similarly, the autonomous agent runtime assists with self-management but does
not replace operator knowledge of storage capacity planning, backup validation,
security policy design, or cluster topology management.

---

## Not Feature-Complete In All Directions Simultaneously

The CDE design makes tradeoffs. By converging many concerns into one engine,
it accepts certain constraints:

- The native language (SBsql) is the most complete interface. Compatibility
  surfaces are scoped.
- The MGA recheck invariant means the engine always rechecks results from
  specialized providers — this is correct but not free; it is a deliberate
  correctness choice.
- The parser-engine separation means compatibility parsers run out-of-process
  — this is a security choice that has overhead implications.
- The autonomous agent runtime is conservative by default — agents start in
  observe or dry-run modes because unexpected autonomous behavior is worse
  than requiring an operator to promote an agent.

These tradeoffs are intentional. The system is not trying to be the fastest
single-model engine at any particular workload; it is trying to be a correct,
governable, convergent engine across many workloads. Knowing the tradeoffs lets
you decide whether ScratchBird is the right tool for your use case.

---

## Summary Of Non-Goals

| Non-goal | What the system does instead |
|----------|------------------------------|
| Universal dialect coverage | Scoped compatibility with explicit diagnostic refusal at scope boundary |
| Automatic compatibility activation | Opt-in per-database configuration |
| Guaranteed feature completeness in all builds | Build-dependent capability with diagnostic surfaces to verify |
| Silent approximation or substitution | Explicit refusal with structured diagnostic |
| Production or security certification | Architectural basis for independent assessment |
| Replace operator expertise | Assist and govern autonomous tasks; require informed configuration |
| Maximum single-workload performance | Correct, governable convergence across many workloads |
