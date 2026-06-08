# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# Current-syntax facade that keeps the lane-local `scratchbird` module on the
# active native bootstrap runtime surface while transport cutover proceeds.

from collections import List
from scratchbird_native import ScratchBirdConfig
from scratchbird_native import ScratchBirdConnection
from scratchbird_native import ScratchBirdStatement
from scratchbird_native import ScratchBirdStream
from scratchbird_native import connect
from scratchbird_native import extract_sqlstate
from scratchbird_native import normalize_metadata_collection_name
from scratchbird_native import normalize_metadata_restriction_key
from scratchbird_native import resolve_metadata_collection_query
from scratchbird_native import resolve_metadata_collection_query_restricted
from scratchbird_native import resolve_metadata_collection_query_restricted_multi
from scratchbird_native import validate_connect_guards
from scratchbird_native import METADATA_SCHEMAS_QUERY
from scratchbird_native import METADATA_TABLES_QUERY
from scratchbird_native import METADATA_COLUMNS_QUERY
from scratchbird_native import METADATA_INDEXES_QUERY
from scratchbird_native import METADATA_INDEX_COLUMNS_QUERY
from scratchbird_native import METADATA_CONSTRAINTS_QUERY
from scratchbird_native import METADATA_PROCEDURES_QUERY
from scratchbird_native import METADATA_FUNCTIONS_QUERY
from scratchbird_native import METADATA_ROUTINES_QUERY
from scratchbird_native import METADATA_CATALOGS_QUERY
from scratchbird_native import METADATA_PRIMARY_KEYS_QUERY
from scratchbird_native import METADATA_FOREIGN_KEYS_QUERY
from scratchbird_native import METADATA_TABLE_PRIVILEGES_QUERY
from scratchbird_native import METADATA_COLUMN_PRIVILEGES_QUERY
from scratchbird_native import METADATA_TYPE_INFO_QUERY

comptime DEFAULT_METADATA_COLLECTION = "tables"


fn schemas_query() -> String:
    return METADATA_SCHEMAS_QUERY


fn tables_query() -> String:
    return METADATA_TABLES_QUERY


fn columns_query() -> String:
    return METADATA_COLUMNS_QUERY


fn indexes_query() -> String:
    return METADATA_INDEXES_QUERY


fn index_columns_query() -> String:
    return METADATA_INDEX_COLUMNS_QUERY


fn constraints_query() -> String:
    return METADATA_CONSTRAINTS_QUERY


fn procedures_query() -> String:
    return METADATA_PROCEDURES_QUERY


fn functions_query() -> String:
    return METADATA_FUNCTIONS_QUERY


fn routines_query() -> String:
    return METADATA_ROUTINES_QUERY


fn catalogs_query() -> String:
    return METADATA_CATALOGS_QUERY


fn primary_keys_query() -> String:
    return METADATA_PRIMARY_KEYS_QUERY


fn foreign_keys_query() -> String:
    return METADATA_FOREIGN_KEYS_QUERY


fn table_privileges_query() -> String:
    return METADATA_TABLE_PRIVILEGES_QUERY


fn column_privileges_query() -> String:
    return METADATA_COLUMN_PRIVILEGES_QUERY


fn type_info_query() -> String:
    return METADATA_TYPE_INFO_QUERY


fn metadata_query(collection_name: String = DEFAULT_METADATA_COLLECTION) raises -> String:
    return resolve_metadata_collection_query(collection_name)


fn metadata_query_restricted(
    collection_name: String = DEFAULT_METADATA_COLLECTION,
    restriction_key: String = "",
    restriction_value: String = "",
) raises -> String:
    return resolve_metadata_collection_query_restricted(
        collection_name,
        restriction_key,
        restriction_value,
    )


fn metadata_query_restricted_multi(
    collection_name: String,
    restriction_keys: List[String],
    restriction_values: List[String],
) raises -> String:
    return resolve_metadata_collection_query_restricted_multi(
        collection_name,
        restriction_keys,
        restriction_values,
    )
