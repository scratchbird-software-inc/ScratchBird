# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird metadata helper queries

sb_metadata_schemas_query <- function() {
  "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
}

sb_metadata_tables_query <- function() {
  "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
}

sb_metadata_columns_query <- function() {
  paste(
    "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression",
    "FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
  )
}

sb_metadata_indexes_query <- function() {
  "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
}

sb_metadata_index_columns_query <- function() {
  "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
}

sb_metadata_constraints_query <- function() {
  "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
}

sb_metadata_procedures_query <- function() {
  "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS procedure_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'procedure' ORDER BY schema_name, procedure_name"
}

sb_metadata_functions_query <- function() {
  "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS function_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'function' ORDER BY schema_name, function_name"
}

SB_METADATA_SCHEMA_FIELD_CANDIDATES <- c(
  "schema_name",
  "table_schem",
  "table_schema",
  "schema"
)

sb_metadata_schema_paths_for_navigation <- function(schema_names, expand_schema_parents = FALSE) {
  raw_paths <- sb_metadata_extract_schema_values(schema_names)
  out <- character()
  seen <- new.env(parent = emptyenv())

  for (raw_path in raw_paths) {
    normalized <- sb_metadata_normalize_schema_path(raw_path)
    if (is.null(normalized)) next

    if (!isTRUE(expand_schema_parents)) {
      out <- sb_metadata_append_unique_path(out, seen, normalized)
      next
    }

    current <- ""
    for (segment in sb_metadata_split_schema_path(normalized)) {
      current <- if (current == "") segment else paste(current, segment, sep = ".")
      out <- sb_metadata_append_unique_path(out, seen, current)
    }
  }

  out
}

sb_metadata_build_schema_tree <- function(schema_names, database = "", expand_schema_parents = FALSE) {
  schema_paths <- sb_metadata_schema_paths_for_navigation(
    schema_names,
    expand_schema_parents = expand_schema_parents
  )

  terminal_paths <- new.env(parent = emptyenv())
  for (path in schema_paths) {
    assign(sb_metadata_seen_key(path), TRUE, envir = terminal_paths)
  }

  nodes_by_path <- new.env(parent = emptyenv())
  roots <- list()

  for (schema_path in schema_paths) {
    segments <- sb_metadata_split_schema_path(schema_path)
    if (length(segments) == 0) next

    parent <- NULL
    current <- ""
    for (segment in segments) {
      current <- if (current == "") segment else paste(current, segment, sep = ".")
      node_key <- sb_metadata_seen_key(current)
      if (exists(node_key, envir = nodes_by_path, inherits = FALSE)) {
        node <- get(node_key, envir = nodes_by_path, inherits = FALSE)
      } else {
        node <- sb_metadata_new_tree_node(segment, current)
        assign(node_key, node, envir = nodes_by_path)
        if (is.null(parent)) {
          roots[[length(roots) + 1]] <- node
        } else {
          parent$children[[length(parent$children) + 1]] <- node
        }
      }

      if (exists(node_key, envir = terminal_paths, inherits = FALSE)) {
        node$terminal <- TRUE
      }
      parent <- node
    }
  }

  list(
    database = sb_metadata_normalize_database_name(database),
    schemas = lapply(roots, sb_metadata_tree_node_to_list)
  )
}

sb_metadata_build_schema_tree_rows <- function(schema_names, database = "", expand_schema_parents = FALSE) {
  tree <- sb_metadata_build_schema_tree(
    schema_names,
    database = database,
    expand_schema_parents = expand_schema_parents
  )
  database_name <- tree$database
  if (is.null(database_name)) database_name <- "default"

  rows <- list(list(
    kind = "database",
    database = database_name,
    parent_path = "",
    path = database_name,
    name = database_name,
    terminal = FALSE,
    top_level_branch = FALSE
  ))

  for (root in tree$schemas) {
    rows <- sb_metadata_append_tree_rows(root, database_name, database_name, TRUE, rows)
  }

  sb_metadata_rows_to_df(rows)
}

sb_metadata_extract_schema_values <- function(schema_names) {
  if (is.null(schema_names)) return(character())

  if (is.character(schema_names) || is.factor(schema_names)) {
    return(as.character(schema_names))
  }

  if (is.data.frame(schema_names)) {
    key <- sb_metadata_find_schema_key(names(schema_names))
    if (is.null(key)) return(character())
    return(as.character(schema_names[[key]]))
  }

  if (is.list(schema_names)) {
    out <- character()
    for (entry in schema_names) {
      value <- sb_metadata_read_schema_value(entry)
      if (!is.null(value)) out <- c(out, value)
    }
    return(out)
  }

  character()
}

sb_metadata_read_schema_value <- function(entry) {
  if (is.null(entry)) return(NULL)

  if (is.character(entry) || is.factor(entry)) {
    return(sb_metadata_to_scalar_string(as.character(entry)))
  }

  if (is.data.frame(entry)) {
    key <- sb_metadata_find_schema_key(names(entry))
    if (is.null(key)) return(NULL)
    return(sb_metadata_to_scalar_string(as.character(entry[[key]])))
  }

  if (!is.list(entry)) return(NULL)
  key <- sb_metadata_find_schema_key(names(entry))
  if (is.null(key)) return(NULL)
  sb_metadata_to_scalar_string(entry[[key]])
}

sb_metadata_find_schema_key <- function(keys) {
  if (is.null(keys) || length(keys) == 0) return(NULL)
  lowered <- tolower(keys)
  for (candidate in SB_METADATA_SCHEMA_FIELD_CANDIDATES) {
    idx <- match(candidate, lowered, nomatch = 0L)
    if (idx > 0) return(keys[[idx]])
  }
  NULL
}

sb_metadata_to_scalar_string <- function(value) {
  if (is.null(value) || length(value) == 0) return(NULL)
  scalar <- value[[1]]
  if (is.null(scalar) || length(scalar) == 0 || is.list(scalar) || is.na(scalar)) return(NULL)
  trimmed <- trimws(as.character(scalar))
  if (trimmed == "") return(NULL)
  trimmed
}

sb_metadata_split_schema_path <- function(schema_name) {
  value <- sb_metadata_to_scalar_string(schema_name)
  if (is.null(value)) return(character())
  segments <- trimws(strsplit(value, ".", fixed = TRUE)[[1]])
  segments[segments != ""]
}

sb_metadata_normalize_schema_path <- function(schema_name) {
  segments <- sb_metadata_split_schema_path(schema_name)
  if (length(segments) == 0) return(NULL)
  paste(segments, collapse = ".")
}

sb_metadata_seen_key <- function(value) {
  paste0("path::", value)
}

sb_metadata_append_unique_path <- function(values, seen, path) {
  key <- sb_metadata_seen_key(path)
  if (exists(key, envir = seen, inherits = FALSE)) return(values)
  assign(key, TRUE, envir = seen)
  c(values, path)
}

sb_metadata_normalize_database_name <- function(database) {
  value <- sb_metadata_to_scalar_string(database)
  if (is.null(value)) return(NULL)
  value
}

sb_metadata_new_tree_node <- function(name, path) {
  node <- new.env(parent = emptyenv())
  node$name <- name
  node$path <- path
  node$terminal <- FALSE
  node$children <- list()
  node
}

sb_metadata_tree_node_to_list <- function(node) {
  list(
    name = node$name,
    path = node$path,
    terminal = isTRUE(node$terminal),
    children = lapply(node$children, sb_metadata_tree_node_to_list)
  )
}

sb_metadata_append_tree_rows <- function(node, database_name, parent_path, top_level, rows) {
  rows[[length(rows) + 1]] <- list(
    kind = "schema",
    database = database_name,
    parent_path = parent_path,
    path = node$path,
    name = node$name,
    terminal = isTRUE(node$terminal),
    top_level_branch = isTRUE(top_level)
  )

  for (child in node$children) {
    rows <- sb_metadata_append_tree_rows(child, database_name, node$path, FALSE, rows)
  }

  rows
}

sb_metadata_rows_to_df <- function(rows) {
  if (length(rows) == 0) {
    return(data.frame(
      kind = character(),
      database = character(),
      parent_path = character(),
      path = character(),
      name = character(),
      terminal = logical(),
      top_level_branch = logical(),
      stringsAsFactors = FALSE
    ))
  }

  data.frame(
    kind = vapply(rows, function(row) row$kind, character(1)),
    database = vapply(rows, function(row) row$database, character(1)),
    parent_path = vapply(rows, function(row) row$parent_path, character(1)),
    path = vapply(rows, function(row) row$path, character(1)),
    name = vapply(rows, function(row) row$name, character(1)),
    terminal = vapply(rows, function(row) isTRUE(row$terminal), logical(1)),
    top_level_branch = vapply(rows, function(row) isTRUE(row$top_level_branch), logical(1)),
    stringsAsFactors = FALSE
  )
}
