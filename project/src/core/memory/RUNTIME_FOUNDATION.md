# Core Memory Runtime Foundation

This package implements `RUNTIME-005`: bounded allocation policies, subsystem tags, deterministic allocation failure diagnostics, and allocation tracking.

## Scope

The package owns:

- `AllocationPolicy` with byte limit and failure mode;
- `MemoryTag` with subsystem and purpose;
- `BoundedAllocator` for tracked allocations;
- deterministic diagnostics for zero-sized allocation, invalid alignment, limit exceeded, underlying allocation failure, and unknown pointer release;
- `MemoryAccountingSnapshot` counters for current bytes, peak bytes, allocation count, deallocation count, failure count, and active allocation count.

## Authority rules

- All runtime allocations must carry subsystem/purpose tags once this layer is used by higher packages.
- Bounded allocation failure is deterministic and reported through diagnostics.
- This package does not own page-buffer policy, page alignment contracts, disk allocation, catalog lifetime, transaction lifetime, or parser memory arenas. Those are later slices built on this foundation.

## Page-buffer allocation helpers

`RUNTIME-006` adds page-buffer helpers on top of `BoundedAllocator`.

The page-buffer helper validates:

- page size is a power of two;
- page size is within the initial supported runtime range of 1 KiB through 1 MiB;
- page count is nonzero and cannot overflow byte calculation;
- alignment is a power of two and at least `alignof(std::max_align_t)`;
- default page-buffer alignment is 4096 bytes.

The helper returns a `PageBuffer` with pointer, byte count, page size, page count, and alignment. Release remains allocator-owned so accounting snapshots remain authoritative.
