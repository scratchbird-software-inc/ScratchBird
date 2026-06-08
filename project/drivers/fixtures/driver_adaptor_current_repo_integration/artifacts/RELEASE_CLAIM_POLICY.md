# Release Claim Policy

Search key: `DRIVER_RELEASE_CLAIM_POLICY`

A driver, adaptor, or tool may be declared `supported` only when all applicable
gates pass:

- source inventory;
- old-path gate;
- artifact isolation;
- toolchain detection;
- dependency policy;
- native build/test;
- common conformance for actual drivers;
- adaptor/tool functional contract where applicable;
- package/install smoke where applicable;
- static hygiene;
- final zero-drift audit.

Until those gates pass, release documents must use one of:

- `experimental`
- `toolchain-waived`
- `not-imported`

No benchmark parity claim may cite a driver unless that driver is at least
`experimental` and the benchmark runner evidence uses current-repo paths.
