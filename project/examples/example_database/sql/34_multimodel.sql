-- 34_multimodel.sql
-- Multi-model statement surface exercise for the example ScratchBird database.
-- Only forms confirmed in Language_Reference/syntax_reference/multimodel_statements.md
-- are used.  Each section notes which surface is exercised or why it is omitted.
-- Source authority: multimodel_statements.md, sb_json.md, sb_vector.md.

-- ===========================================================================
-- A. DOCUMENT model: operate on app.catalog.products.attributes (document col)
-- ===========================================================================

-- A1. DOCUMENT QUERY: find products where the 'category' attribute is 'electronics'.
document query app.catalog.products
where path '$.category' = 'electronics'
return product_id,
       path '$.category'    as category,
       path '$.weight_kg'   as weight_kg;

-- A2. DOCUMENT PATCH: add a 'clearance' flag and remove a draft field.
--     Targets the document store via the qualified table name.
document patch app.catalog.products
key 1001
set path '$.clearance' = true,
    path '$.reviewed_at' = current_timestamp
remove path '$.draft_note'
return product_id, path '$.clearance' as clearance_flag;

-- A3. SQL SELECT using json_value() and json_exists() from sb_json package
--     to read document paths inline with a relational query.
select p.product_id,
       p.sku,
       json_value(p.attributes, '$.category')  as category,
       json_value(p.attributes, '$.weight_kg') as weight_kg
from app.catalog.products p
where json_exists(p.attributes, '$.category')
  and p.in_stock = true
order by p.product_id;

-- ===========================================================================
-- B. VECTOR model: search over a vector-bearing table
-- ===========================================================================
-- NOTE: The canonical schema has no dedicated vector table with an embedding
-- column. A VECTOR SEARCH statement is shown here as a self-contained example
-- using the documented syntax from multimodel_statements.md (vector_op_stmt).
-- Replace 'app.product_embedding' with the actual vector table once it is
-- created in this deployment.

-- B1. VECTOR SEARCH with L2 metric and result limit.
--     Uses the documented vector(expr) constructor from sb_vector package.
vector search app.product_embedding
using vector('[0.1,0.2,0.3,0.4,0.8]')
metric l2
limit 10
return object_uuid, distance, recheck_status;

-- B2. Filtered VECTOR SEARCH: restrict to in-stock products.
vector search app.product_embedding
using vector('[0.5,0.1,0.6,0.2,0.9]')
metric cosine
where in_stock = true
limit 5
return object_uuid, distance;

-- B3. SQL-level distance computation using cosine_distance() from sb_vector.
--     cosine_distance(vector,vector) is documented in sb_vector.md.
select cosine_distance(
         vector('[1.0,0.0,0.0]'),
         vector('[0.0,1.0,0.0]')
       ) as angle_distance;

-- ===========================================================================
-- C. SEARCH (full-text) model
-- ===========================================================================
-- NOTE: No full-text search index exists on the canonical schema tables.
-- The SEARCH statement syntax is shown per multimodel_statements.md.
-- Bind 'app.product_search' to an actual search-index object in deployment.

-- C1. Text search with language filter.
search app.product_search
for 'wireless headphones'
where language_tag = 'en'
limit 15
return object_uuid, score, snippet;

-- C2. Fielded search restricted to name and description columns.
search app.product_search
for 'noise cancelling'
fields name, description
limit 10
return object_uuid, score, matched_fields;

-- ===========================================================================
-- D. TIMESERIES model: operate on ts.sensor_reading
-- ===========================================================================

-- D1. TIMESERIES QUERY with 1-minute bucketing and gap detection.
timeseries query ts.sensor_reading
between cast('2025-01-01 00:00:00' as timestamptz)
    and cast('2025-01-01 01:00:00' as timestamptz)
where sensor_id = 'temp_001'
bucket interval '1 minute'
aggregate avg(value) as avg_value, max(value) as max_value
return sensor_id, bucket_start, avg_value, max_value;

-- D2. TIMESERIES GAPFILL: fill missing 5-minute buckets using previous value.
timeseries gapfill ts.sensor_reading
between cast('2025-01-01 00:00:00' as timestamptz)
    and cast('2025-01-01 06:00:00' as timestamptz)
bucket interval '5 minutes'
method previous
return bucket_start, value, gapfill_status;

-- D3. SQL window query on ts.sensor_reading for comparison.
select sensor_id,
       date_trunc('hour', reading_ts)  as hour_bucket,
       avg(value)                      as avg_value,
       min(value)                      as min_value,
       max(value)                      as max_value,
       count(*)                        as sample_count
from ts.sensor_reading
where reading_ts >= cast('2025-01-01 00:00:00' as timestamptz)
  and reading_ts <  cast('2025-01-02 00:00:00' as timestamptz)
group by sensor_id, date_trunc('hour', reading_ts)
order by sensor_id, hour_bucket;

-- ===========================================================================
-- E. GRAPH model
-- ===========================================================================
-- NOTE: No graph object exists in the canonical schema.
-- Graph syntax is shown per multimodel_statements.md (graph_op_stmt).
-- Bind 'app.customer_graph' to an actual graph object in deployment.

-- E1. Pattern match: customers who placed orders.
graph match app.customer_graph
pattern (:customer)-[:placed]->(:order)
where property('customer_id') = 1
return node('order') as order_node,
       path() as matched_path;

-- E2. Traversal with depth bounds.
graph traverse app.customer_graph
from node 1
over edge_type 'placed'
depth 1 to 3
return node_id, edge_id, path_depth;

-- ===========================================================================
-- F. KEY-VALUE model
-- ===========================================================================
-- NOTE: No key-value store object exists in the canonical schema.
-- KV syntax is shown per multimodel_statements.md (keyvalue_op_stmt).
-- Bind 'app.session_cache' to an actual KV store object in deployment.

-- F1. KV GET by key.
kv get app.session_cache
key 'sess:user:42'
return key, value, expires_at;

-- F2. KV PUT with TTL.
kv put app.session_cache
key 'sess:user:42'
value '{"role":"app_reader","ts":1704067200}'
ttl interval '30 minutes'
return key, version, expires_at;

-- F3. KV SCAN over a key prefix range.
kv scan app.session_cache
from key 'sess:'
to key 'sess;'
limit 100
return key, value, version;

-- ===========================================================================
-- G. Mixed multimodel + relational flow
-- ===========================================================================
-- Uses a vector search result as a CTE, then joins to the relational table.
-- Demonstrates the WITH + multimodel rowset form from multimodel_statements.md.

with ranked_products as (
  vector search app.product_embedding
  using vector('[0.3,0.7,0.1,0.5,0.2]')
  metric l2
  limit 20
  return object_uuid, distance
)
select p.product_id,
       p.name,
       p.price,
       ranked_products.distance
from ranked_products
join app.catalog.products p
  on cast(p.product_id as uuid) = ranked_products.object_uuid
where p.in_stock = true
order by ranked_products.distance, p.product_id;
