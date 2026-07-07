ScratchBird public release layout
=================================

Release notes, upgrade notes, migration notes, known limitations, and initial
platform status are declared here for the public engine-only release layout.

Linux is the first validated execution platform for the public native release.
Windows x64 and FreeBSD layouts are declared compatibility targets with
cross-platform proof pending on native runners for each platform. Win32 is out of scope and is not a supported release target.

The public release layout is external-provider-only for cluster execution.
public cluster stubs are compile/link boundaries; they are not local cluster
execution authority, transaction finality authority, visibility authority, or
recovery authority.

Only engine binary families are admitted into these layout definitions. Listener,
manager, parser, driver, UDR, and cluster-provider payloads are excluded from the
engine binary release package and must be proven separately by their own release
lanes.
