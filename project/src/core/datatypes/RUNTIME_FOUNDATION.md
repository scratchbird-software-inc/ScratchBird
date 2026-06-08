# Core Datatypes Runtime Foundation

This package implements `RUNTIME-012`: canonical datatype descriptor authority.

## Scope

The package owns:

- canonical type identifiers;
- type-family classification;
- descriptor-authoritative metadata;
- mandatory capability mapping for `int128`, `uint128`, `real128`, and descriptor-defined decimal support;
- validation that donor names are aliases, not engine authority.

## Authority rules

- The engine sees canonical descriptors, not donor type strings.
- Donor type names are compatibility labels over descriptors.
- Descriptor identity and metadata are engine-owned.
- `int128`, `uint128`, and `real128` are required capabilities, not optional compatibility extras.
- Arithmetic, serialization, cast rules, donor-label mapping, and conversion diagnostics are later slices.

## Capability rules

The descriptor layer references these mandatory runtime capabilities:

- `numeric.int128`;
- `numeric.uint128`;
- `numeric.real128`.

The runtime capability package remains responsible for provider detection. This package only declares which descriptors require which capability.

## RUNTIME-013 descriptor exchange

This package also implements `RUNTIME-013`: deterministic descriptor serialization, donor-label placeholders, and conversion diagnostics.

The exchange layer owns:

- a fixed-size serialized canonical descriptor record for catalog/bootstrap use;
- donor type label placeholder mappings that resolve to canonical descriptors;
- conversion diagnostic classification for exact, widening, narrowing, precision-loss, incompatible, and unsupported paths.

The exchange layer does not make donor labels authoritative. A donor label is parser input only. The parser must resolve the label to a canonical descriptor before engine authority is reached.

The exchange layer does not implement arithmetic, casts, binary value serialization, collation, encoding, or donor-specific precision rules. Those are later slices.
