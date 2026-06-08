# Hibernate Example Set

This folder provides deterministic examples for ECOSYS-403.

- `hibernate.properties.example`: datasource/bootstrap settings for ScratchBird.
- `ScratchBirdEntityLifecycleExample.java`: entity lifecycle sample (persist/find/update/remove with transaction boundaries).
- `migration-mapping.sql`: representative DDL mapping aligned with entity annotations.

These examples are intended as implementation and migration guidance. Runtime
execution requires a live ScratchBird DSN and Hibernate/JPA bootstrap wiring.
