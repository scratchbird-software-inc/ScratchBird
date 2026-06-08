# TypeORM Sample Service Example

`sample-service.js` demonstrates deterministic adapter usage for:

- datasource option normalization and policy guardrails,
- metadata-to-entity schema generation,
- nested relation CRUD transaction plan generation.

To run the sample in real runtime mode, wire the generated entities into a
TypeORM `DataSource` and provide a live ScratchBird DSN.
