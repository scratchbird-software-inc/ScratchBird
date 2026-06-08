# Storage Page Runtime Foundation

This package implements `RUNTIME-010`: the page-family registry and page manager-facing classification result structure.

## Scope

The package owns:

- page-family enum;
- built-in page-family descriptor registry;
- lookup by durable page type;
- page manager classification derived from disk page-header classification;
- read/write capability decisions for page manager callers.

## Page family rules

- Every page type belongs to one page family.
- Cluster-private pages are classifiable but not writable by standalone public-node authority.
- Encrypted or opaque pages are classifiable but require decryption authority before body interpretation.
- Reserved pages are never writable by the generic page manager.
- Unknown unsafe pages fail closed.

## Non-scope

This slice does not implement page body parsing, allocation maps, catalog pages, transaction inventory pages, data pages, index pages, or recovery behavior. Those are later slices.

## RUNTIME-011 page skeletons

This package also implements `RUNTIME-011`: the first page-family skeleton contract for page manager callers.

The skeleton registry defines the initial page body categories required by the runtime foundation:

- database header page;
- allocation map page;
- catalog page;
- transaction inventory page;
- row data page;
- index B-tree page body with engine-owned key payload encoding.

The skeleton layer is intentionally conservative:

- page headers and page-family classification must succeed before body classification is considered;
- every engine identity UUID requirement means UUIDv7 only;
- UUIDv1 through UUIDv6 values are donor/client compatibility values only and must be mapped before engine authority;
- unsupported page types fail closed;
- cluster-private and encrypted pages remain blocked by the page registry authority checks;
- page body parsing and mutation are explicitly marked unavailable until their own slices implement them;
- index pages use the `index_btree` page body for initial non-cluster storage.

This slice reserves the page-manager contract; it does not define final on-disk body layouts.

## DBOPEN-002 managed page headers

This package now includes the first managed page-header API for the database create/open vertical slice.

The managed page-header layer owns:

- page offset calculation from page number and page-size profile;
- page manager context validation for database UUIDv7 and filespace UUIDv7;
- creation of managed page headers through the disk page-header serializer;
- validation of managed serialized page headers against database/filespace identity;
- page skeleton classification after page-header validation.

This API still does not define final page body layouts or recovery behavior.
