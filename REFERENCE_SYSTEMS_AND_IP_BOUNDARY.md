# IP Boundary

ScratchBird uses external database systems for behavior research, compatibility testing, and operational comparison.

## Public Boundary

- ScratchBird implementation source is authored in this repository and is not derived from external database implementation source.
- External implementation source, parser implementation source, and external database documentation are not part of the public GitHub source surface.
- Raw upstream regression payloads, native tools, binaries, utilities, external source trees, and external database documentation are not tracked in this public repository.
- ScratchBird-owned CTest harnesses, fixture locators, manifest files, acquisition metadata, and boundary gates may be tracked so an operator can attach externally obtained reference fixtures during validation.
- Externally obtained fixtures and regression suites remain governed by their upstream licenses and notices.

## Compatibility Research

External systems may be used for:

- behavioral comparison;
- compatibility matrices;
- parser and protocol refusal tests;
- external tool interoperability tests;
- regression corpus replay through externally supplied fixtures where licensing permits.

External systems are not used as:

- ScratchBird execution authority;
- ScratchBird storage, recovery, security, or transaction authority;
- a source of copied implementation code;
- evidence of production support by themselves.

## Release Review Rule

The public repository must not track external implementation source, external database documentation, raw upstream regression payloads, or downloaded/native reference tools. Release gates enforce that boundary while allowing ScratchBird-owned compatibility harnesses, manifests, locators, and refusal tests.
