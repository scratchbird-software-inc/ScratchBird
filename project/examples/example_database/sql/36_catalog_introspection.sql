-- 36_catalog_introspection.sql
-- Catalog and system introspection queries for the example ScratchBird database.
-- Uses only: (a) SHOW statements from management_and_operations.md, and
--   (b) SELECT from the catalog surfaces documented in
--   Language_Reference/catalog_reference/ (namespace sys.catalog.* and
--   sys.security.*).
-- Column names are taken directly from the catalog reference pages.
-- Source authority: management_and_operations.md, catalog_reference/index.md,
--   sys_catalog_type_descriptor.md, sys_catalog_domain_descriptor.md,
--   sys_catalog_operation_descriptor.md,
--   sys_security_protected_material_catalog.md.

-- ===========================================================================
-- A. SHOW statements  (from management_and_operations.md)
-- ===========================================================================

-- Service health, readiness, and liveness.
show health;
show readiness;
show liveness;

-- Build and runtime information.
show version;
show build;
show runtime;

-- Session and transaction state.
show management sessions;
show management transactions;
show management statements;

-- Resource state.
show management storage;
show management memory;
show management cache;
show management indexes;

-- Configuration.
show management config effective;

-- Schema and object metadata (SHOW DESCRIBE forms from security page).
show roles;
show grants;
show policies;
show masks;
show rls;

-- Current database.
describe database current;

-- ===========================================================================
-- B. sys.catalog.type_descriptor
--    Documents canonical type carrier identity.
--    Columns: descriptor_uuid, canonical_type, type_family
--    (plus nullable UUID columns confirmed in sys_catalog_type_descriptor.md).
-- ===========================================================================

select descriptor_uuid,
       canonical_type,
       type_family
from sys.catalog.type_descriptor
order by type_family, canonical_type;

-- Count types per family.
select type_family,
       count(*) as type_count
from sys.catalog.type_descriptor
group by type_family
order by type_family;

-- Look up the descriptor for the bigint carrier (used by our PK columns).
select descriptor_uuid,
       canonical_type,
       type_family,
       source_type_uuid,
       modifier_profile_uuid
from sys.catalog.type_descriptor
where canonical_type = 'bigint';

-- ===========================================================================
-- C. sys.catalog.domain_descriptor
--    Documents domain identity, base carrier, and policy references.
--    Columns: domain_uuid, domain_kind, base_descriptor_uuid,
--             base_domain_uuid, nullable_policy_uuid, source_type_name
--    (confirmed from sys_catalog_domain_descriptor.md).
-- ===========================================================================

-- All domains visible to the caller.
select domain_uuid,
       domain_kind,
       base_descriptor_uuid,
       base_domain_uuid,
       source_type_name
from sys.catalog.domain_descriptor
order by source_type_name;

-- Inspect the three application domains created in 11_domains.sql.
select domain_uuid,
       domain_kind,
       source_type_name,
       nullable_policy_uuid,
       constraint_set_uuid,
       cast_policy_uuid
from sys.catalog.domain_descriptor
where source_type_name in ('app.email_addr', 'app.positive_qty', 'app.money')
order by source_type_name;

-- ===========================================================================
-- D. sys.catalog.domain_element
--    Compound-domain element policy entries.
--    Columns: domain_uuid, element_index, element_name, element_descriptor_uuid
--    (confirmed from sys_catalog_domain_element.md via catalog_reference/index.md).
-- ===========================================================================

select domain_uuid,
       element_index,
       element_name,
       element_descriptor_uuid
from sys.catalog.domain_element
order by domain_uuid, element_index;

-- ===========================================================================
-- E. sys.catalog.type_capability
--    Links a descriptor to its capability profile.
--    Columns: capability_uuid, descriptor_uuid, capability_class
--    (confirmed from sys_catalog_type_capability.md).
-- ===========================================================================

select tc.capability_uuid,
       tc.descriptor_uuid,
       td.canonical_type,
       tc.capability_class
from sys.catalog.type_capability tc
join sys.catalog.type_descriptor td
  on td.descriptor_uuid = tc.descriptor_uuid
order by td.canonical_type, tc.capability_class
limit 50;

-- ===========================================================================
-- F. sys.catalog.operation_descriptor
--    Records SBLR-admitted operations and their descriptor/cost metadata.
--    Columns: operation_uuid, operation_kind, determinism_class, cost_class
--    (confirmed from sys_catalog_operation_descriptor.md).
-- ===========================================================================

-- Summary of operation kinds.
select operation_kind,
       count(*)          as op_count
from sys.catalog.operation_descriptor
group by operation_kind
order by operation_kind;

-- Volatile or side-effecting operations (useful for security audit).
select operation_uuid,
       operation_kind,
       determinism_class,
       cost_class
from sys.catalog.operation_descriptor
where determinism_class in ('volatile', 'side_effecting')
order by operation_kind, operation_uuid
limit 20;

-- ===========================================================================
-- G. sys.catalog.reference_type_mapping  (donor type / compatibility mapping)
--    Columns documented in sys_catalog_donor_type_mapping.md.
--    Surface name used here is the alias visible in catalog_reference/index.md:
--    sys.catalog.reference_type_mapping
-- ===========================================================================

select *
from sys.catalog.reference_type_mapping
order by 1
limit 20;

-- ===========================================================================
-- H. sys.security.protected_material_catalog
--    Metadata about protected material; never exposes raw secrets.
--    Columns: protected_material_uuid, object_class, purpose_class,
--             storage_class, lifecycle_state, security_epoch
--    (confirmed from sys_security_protected_material_catalog.md).
-- ===========================================================================

select protected_material_uuid,
       object_class,
       purpose_class,
       storage_class,
       lifecycle_state,
       security_epoch
from sys.security.protected_material_catalog
order by lifecycle_state, object_class;

-- ===========================================================================
-- I. sys.security.protected_material_policy_binding
--    Links protected material to policy bindings.
--    Confirmed in catalog_reference/index.md as a documented surface.
-- ===========================================================================

select *
from sys.security.protected_material_policy_binding
limit 20;

-- ===========================================================================
-- J. sys.security.protected_material_audit
--    Audit evidence for protected material access events (read-only, redacted).
--    Confirmed in catalog_reference/index.md as a documented surface.
-- ===========================================================================

select *
from sys.security.protected_material_audit
order by 1 desc
limit 20;

-- ===========================================================================
-- K. SHOW FUNCTION / PROCEDURE / TRIGGER  (lifecycle inspection)
--    Documented in function.md, procedure.md, trigger.md as SHOW/DESCRIBE
--    forms within the lifecycle surface.
-- ===========================================================================

show functions;
show procedures;
show triggers;

describe function  app.order_line_total;
describe function  app.format_customer_label;
describe procedure app.recompute_order_total;
describe procedure app.list_customer_orders;
describe trigger   app.sales.orders_ai;
describe trigger   app.sales.orders_au;
