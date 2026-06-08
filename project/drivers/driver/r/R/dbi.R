# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

Scratchbird <- function() {
  new("ScratchbirdDriver")
}

setClass("ScratchbirdDriver", contains = "DBIDriver")
setClass("ScratchbirdConnection", contains = "DBIConnection", slots = list(ptr = "environment"))
setClass("ScratchbirdResult", contains = "DBIResult", slots = list(result = "environment"))

setMethod("dbConnect", "ScratchbirdDriver", function(drv, dsn = "", ...) {
  client <- sb_connect(dsn, ...)
  env <- new.env(parent = emptyenv())
  env$client <- client
  new("ScratchbirdConnection", ptr = env)
})

setMethod("dbCanConnect", "ScratchbirdDriver", function(drv, dsn = "", ...) {
  client <- tryCatch(
    sb_connect(dsn, ...),
    error = function(e) NULL
  )
  if (is.null(client)) {
    return(FALSE)
  }
  try(sb_disconnect(client), silent = TRUE)
  TRUE
})

setMethod("dbDisconnect", "ScratchbirdConnection", function(conn, ...) {
  sb_disconnect(conn@ptr$client)
  TRUE
})

setMethod("dbIsValid", "ScratchbirdConnection", function(dbObj, ...) {
  sb_is_valid(dbObj@ptr$client)
})

setMethod("dbBegin", "ScratchbirdConnection", function(conn, ...) {
  sb_begin(conn@ptr$client, ...)
  sb_set_autocommit(conn@ptr$client, FALSE)
  TRUE
})

setMethod("dbCommit", "ScratchbirdConnection", function(conn, ...) {
  sb_commit(conn@ptr$client, ...)
  sb_set_autocommit(conn@ptr$client, TRUE)
  TRUE
})

setMethod("dbRollback", "ScratchbirdConnection", function(conn, ...) {
  sb_rollback(conn@ptr$client, ...)
  sb_set_autocommit(conn@ptr$client, TRUE)
  TRUE
})

setMethod("dbSendQuery", c("ScratchbirdConnection", "character"), function(conn, statement, ...) {
  result <- sb_send_query(conn@ptr$client, statement, ...)
  new("ScratchbirdResult", result = result)
})

setMethod("dbFetch", "ScratchbirdResult", function(res, n = -1, ...) {
  sb_fetch(res@result, n)
})

setMethod("dbColumnInfo", "ScratchbirdResult", function(res, ...) {
  sb_prime_result_metadata(res@result)
  columns <- res@result$columns
  if (length(columns) == 0) {
    return(data.frame(
      name = character(),
      type_oid = integer(),
      type_size = integer(),
      type_modifier = integer(),
      table_oid = integer(),
      column_index = integer(),
      format = integer(),
      nullable = logical(),
      stringsAsFactors = FALSE
    ))
  }

  data.frame(
    name = vapply(columns, function(col) if (!is.null(col$name)) as.character(col$name) else "", character(1)),
    type_oid = vapply(columns, function(col) as.integer(if (is.null(col$type_oid)) NA_integer_ else col$type_oid), integer(1)),
    type_size = vapply(columns, function(col) as.integer(if (is.null(col$type_size)) NA_integer_ else col$type_size), integer(1)),
    type_modifier = vapply(columns, function(col) as.integer(if (is.null(col$type_modifier)) NA_integer_ else col$type_modifier), integer(1)),
    table_oid = vapply(columns, function(col) as.integer(if (is.null(col$table_oid)) NA_integer_ else col$table_oid), integer(1)),
    column_index = vapply(columns, function(col) as.integer(if (is.null(col$column_index)) NA_integer_ else col$column_index), integer(1)),
    format = vapply(columns, function(col) as.integer(if (is.null(col$format)) NA_integer_ else col$format), integer(1)),
    nullable = vapply(columns, function(col) isTRUE(col$nullable), logical(1)),
    stringsAsFactors = FALSE
  )
})

setMethod("dbClearResult", "ScratchbirdResult", function(res, ...) {
  sb_clear_result(res@result)
  TRUE
})

setMethod("dbGetRowsAffected", "ScratchbirdResult", function(res, ...) {
  as.numeric(res@result$rowcount)
})

setMethod("dbGetQuery", c("ScratchbirdConnection", "character"), function(conn, statement, ...) {
  sb_get_query(conn@ptr$client, statement, ...)
})

setMethod("dbExecute", c("ScratchbirdConnection", "character"), function(conn, statement, ...) {
  result <- sb_send_query(conn@ptr$client, statement, ...)
  sb_fetch_rows(result, -1)
  as.integer(result$rowcount)
})

setMethod("dbListTables", "ScratchbirdConnection", function(conn, ...) {
  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  if (!is.data.frame(tables) || nrow(tables) == 0) {
    return(character())
  }

  table_col <- sb_find_metadata_col(tables, c("table_name"))
  if (is.na(table_col)) {
    return(character())
  }

  schema_col <- sb_find_metadata_col(
    tables,
    c("schema_name", "table_schem", "table_schema", "schema")
  )

  table_names <- as.character(tables[[table_col]])
  if (is.na(schema_col)) {
    return(unique(table_names))
  }

  schema_names <- as.character(tables[[schema_col]])
  qualified <- ifelse(
    is.na(schema_names) | trimws(schema_names) == "",
    table_names,
    paste0(schema_names, ".", table_names)
  )
  unique(qualified)
})

setMethod("dbExistsTable", c("ScratchbirdConnection", "character"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(FALSE)
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  matches <- sb_filter_tables_for_ref(tables, ref)
  nrow(matches) > 0
})

setMethod("dbExistsTable", c("ScratchbirdConnection", "Id"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(FALSE)
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  matches <- sb_filter_tables_for_ref(tables, ref)
  nrow(matches) > 0
})

setMethod("dbExistsTable", c("ScratchbirdConnection", "SQL"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(FALSE)
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  matches <- sb_filter_tables_for_ref(tables, ref)
  nrow(matches) > 0
})

setMethod("dbListFields", c("ScratchbirdConnection", "character"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(character())
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  table_matches <- sb_filter_tables_for_ref(tables, ref)
  if (!is.data.frame(table_matches) || nrow(table_matches) == 0) {
    return(character())
  }

  table_id_col <- sb_find_metadata_col(table_matches, c("table_id"))
  if (is.na(table_id_col)) {
    return(character())
  }
  table_ids <- unique(as.character(table_matches[[table_id_col]]))
  if (length(table_ids) == 0) {
    return(character())
  }

  columns <- sb_get_query(conn@ptr$client, sb_metadata_columns_query())
  if (!is.data.frame(columns) || nrow(columns) == 0) {
    return(character())
  }

  columns_table_id_col <- sb_find_metadata_col(columns, c("table_id"))
  column_name_col <- sb_find_metadata_col(columns, c("column_name"))
  if (is.na(columns_table_id_col) || is.na(column_name_col)) {
    return(character())
  }

  mask <- as.character(columns[[columns_table_id_col]]) %in% table_ids
  filtered <- columns[mask, , drop = FALSE]
  if (nrow(filtered) == 0) {
    return(character())
  }

  ordinal_col <- sb_find_metadata_col(filtered, c("ordinal_position"))
  if (!is.na(ordinal_col)) {
    filtered <- filtered[order(as.numeric(filtered[[ordinal_col]])), , drop = FALSE]
  }
  unique(as.character(filtered[[column_name_col]]))
})

setMethod("dbListFields", c("ScratchbirdConnection", "Id"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(character())
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  table_matches <- sb_filter_tables_for_ref(tables, ref)
  if (!is.data.frame(table_matches) || nrow(table_matches) == 0) {
    return(character())
  }

  table_id_col <- sb_find_metadata_col(table_matches, c("table_id"))
  if (is.na(table_id_col)) {
    return(character())
  }
  table_ids <- unique(as.character(table_matches[[table_id_col]]))
  if (length(table_ids) == 0) {
    return(character())
  }

  columns <- sb_get_query(conn@ptr$client, sb_metadata_columns_query())
  if (!is.data.frame(columns) || nrow(columns) == 0) {
    return(character())
  }

  columns_table_id_col <- sb_find_metadata_col(columns, c("table_id"))
  column_name_col <- sb_find_metadata_col(columns, c("column_name"))
  if (is.na(columns_table_id_col) || is.na(column_name_col)) {
    return(character())
  }

  mask <- as.character(columns[[columns_table_id_col]]) %in% table_ids
  filtered <- columns[mask, , drop = FALSE]
  if (nrow(filtered) == 0) {
    return(character())
  }

  ordinal_col <- sb_find_metadata_col(filtered, c("ordinal_position"))
  if (!is.na(ordinal_col)) {
    filtered <- filtered[order(as.numeric(filtered[[ordinal_col]])), , drop = FALSE]
  }
  unique(as.character(filtered[[column_name_col]]))
})

setMethod("dbListFields", c("ScratchbirdConnection", "SQL"), function(conn, name, ...) {
  ref <- sb_normalize_table_ref(name)
  if (is.null(ref$table)) {
    return(character())
  }

  tables <- sb_metadata_tables_with_schema(conn@ptr$client)
  table_matches <- sb_filter_tables_for_ref(tables, ref)
  if (!is.data.frame(table_matches) || nrow(table_matches) == 0) {
    return(character())
  }

  table_id_col <- sb_find_metadata_col(table_matches, c("table_id"))
  if (is.na(table_id_col)) {
    return(character())
  }
  table_ids <- unique(as.character(table_matches[[table_id_col]]))
  if (length(table_ids) == 0) {
    return(character())
  }

  columns <- sb_get_query(conn@ptr$client, sb_metadata_columns_query())
  if (!is.data.frame(columns) || nrow(columns) == 0) {
    return(character())
  }

  columns_table_id_col <- sb_find_metadata_col(columns, c("table_id"))
  column_name_col <- sb_find_metadata_col(columns, c("column_name"))
  if (is.na(columns_table_id_col) || is.na(column_name_col)) {
    return(character())
  }

  mask <- as.character(columns[[columns_table_id_col]]) %in% table_ids
  filtered <- columns[mask, , drop = FALSE]
  if (nrow(filtered) == 0) {
    return(character())
  }

  ordinal_col <- sb_find_metadata_col(filtered, c("ordinal_position"))
  if (!is.na(ordinal_col)) {
    filtered <- filtered[order(as.numeric(filtered[[ordinal_col]])), , drop = FALSE]
  }
  unique(as.character(filtered[[column_name_col]]))
})

sb_find_metadata_col <- function(df, candidates) {
  if (!is.data.frame(df) || length(candidates) == 0) {
    return(NA_character_)
  }
  lowered <- tolower(names(df))
  for (candidate in tolower(candidates)) {
    idx <- match(candidate, lowered, nomatch = 0L)
    if (idx > 0) {
      return(names(df)[[idx]])
    }
  }
  NA_character_
}

sb_scalar_string <- function(value) {
  if (is.null(value) || length(value) == 0) {
    return(NULL)
  }
  scalar <- value[[1]]
  if (is.null(scalar) || length(scalar) == 0 || is.list(scalar)) {
    return(NULL)
  }
  out <- trimws(as.character(scalar))
  if (identical(out, "") || is.na(out)) {
    return(NULL)
  }
  out
}

sb_normalize_table_ref <- function(name) {
  if (inherits(name, "Id")) {
    parts <- as.list(methods::slot(name, "name"))
    names(parts) <- names(methods::slot(name, "name"))
    if (length(parts) == 0) {
      return(list(schema = NULL, table = NULL))
    }
    part_names <- names(parts)
    lowered <- tolower(if (is.null(part_names)) rep("", length(parts)) else part_names)

    read_part <- function(candidate) {
      idx <- match(candidate, lowered, nomatch = 0L)
      if (idx > 0) parts[[idx]] else NULL
    }

    table <- sb_scalar_string(read_part("table"))
    if (is.null(table)) {
      table <- sb_scalar_string(read_part("name"))
    }
    if (is.null(table)) {
      table <- sb_scalar_string(parts[[length(parts)]])
    }

    schema <- sb_scalar_string(read_part("schema"))
    if (is.null(schema) && length(parts) > 1) {
      schema <- sb_scalar_string(parts[[length(parts) - 1]])
    }
    return(list(schema = schema, table = table))
  }

  if (is.list(name)) {
    parts <- as.list(name)
    if (length(parts) == 0) {
      return(list(schema = NULL, table = NULL))
    }
    part_names <- names(parts)
    lowered <- tolower(if (is.null(part_names)) rep("", length(parts)) else part_names)

    read_part <- function(candidate) {
      idx <- match(candidate, lowered, nomatch = 0L)
      if (idx > 0) parts[[idx]] else NULL
    }

    table <- sb_scalar_string(read_part("table"))
    if (is.null(table)) {
      table <- sb_scalar_string(read_part("name"))
    }
    if (is.null(table)) {
      table <- sb_scalar_string(parts[[length(parts)]])
    }

    schema <- sb_scalar_string(read_part("schema"))
    if (is.null(schema) && length(parts) > 1) {
      schema <- sb_scalar_string(parts[[length(parts) - 1]])
    }
    return(list(schema = schema, table = table))
  }

  scalar <- sb_scalar_string(name)
  if (is.null(scalar)) {
    return(list(schema = NULL, table = NULL))
  }

  segments <- strsplit(scalar, ".", fixed = TRUE)[[1]]
  segments <- trimws(segments)
  segments <- segments[segments != ""]
  if (length(segments) >= 2) {
    return(list(
      schema = paste(segments[seq_len(length(segments) - 1)], collapse = "."),
      table = segments[[length(segments)]]
    ))
  }

  list(schema = NULL, table = scalar)
}

sb_metadata_tables_with_schema <- function(client) {
  tables <- sb_get_query(client, sb_metadata_tables_query())
  if (!is.data.frame(tables) || nrow(tables) == 0) {
    return(data.frame())
  }

  schema_id_col <- sb_find_metadata_col(tables, c("schema_id"))
  if (is.na(schema_id_col)) {
    return(tables)
  }

  schemas <- sb_get_query(client, sb_metadata_schemas_query())
  if (!is.data.frame(schemas) || nrow(schemas) == 0) {
    return(tables)
  }

  schema_table_id_col <- sb_find_metadata_col(schemas, c("schema_id"))
  schema_name_col <- sb_find_metadata_col(
    schemas,
    c("schema_name", "table_schem", "table_schema", "schema")
  )
  if (is.na(schema_table_id_col) || is.na(schema_name_col)) {
    return(tables)
  }

  map <- setNames(
    as.character(schemas[[schema_name_col]]),
    as.character(schemas[[schema_table_id_col]])
  )
  tables$schema_name <- unname(map[as.character(tables[[schema_id_col]])])
  tables
}

sb_filter_tables_for_ref <- function(tables, ref) {
  if (!is.data.frame(tables) || nrow(tables) == 0 || is.null(ref$table)) {
    return(data.frame())
  }

  table_col <- sb_find_metadata_col(tables, c("table_name"))
  if (is.na(table_col)) {
    return(data.frame())
  }

  table_mask <- tolower(as.character(tables[[table_col]])) == tolower(ref$table)
  filtered <- tables[table_mask, , drop = FALSE]
  if (nrow(filtered) == 0 || is.null(ref$schema)) {
    return(filtered)
  }

  schema_col <- sb_find_metadata_col(
    filtered,
    c("schema_name", "table_schem", "table_schema", "schema")
  )
  if (is.na(schema_col)) {
    return(data.frame())
  }

  schema_mask <- tolower(as.character(filtered[[schema_col]])) == tolower(ref$schema)
  filtered[schema_mask, , drop = FALSE]
}
