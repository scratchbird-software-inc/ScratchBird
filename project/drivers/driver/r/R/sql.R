# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# Return the new terminator string if `chunk` is a `SET TERM <terminator>`
# client directive, else NULL. Leading full-line `--` comments and blank lines
# are ignored when matching, so a directive may be preceded by comment lines in
# the same chunk.
chunk_set_term <- function(chunk) {
  lines <- strsplit(chunk, "\n", fixed = TRUE)[[1]]
  meaningful <- character(0)
  for (line in lines) {
    stripped <- trimws(line)
    if (!nzchar(stripped) || startsWith(stripped, "--")) next
    meaningful <- c(meaningful, stripped)
  }
  if (length(meaningful) == 0) return(NULL)
  joined <- paste(meaningful, collapse = " ")
  m <- regmatches(joined, regexec("^[Ss][Ee][Tt]\\s+[Tt][Ee][Rr][Mm]\\s+(\\S.*?)\\s*$", joined))[[1]]
  if (length(m) < 2) return(NULL)
  rest <- trimws(m[2])
  if (!nzchar(rest)) return(NULL)
  rest
}

# Split SQL into top-level statements on the active terminator.
#
# Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
# `SET TERM <terminator>` client directive: the
# directive changes the active terminator and is consumed -- it is not emitted as
# a statement and is not counted. This lets procedural bodies contain inner `;`
# between `SET TERM ^` and the restoring `SET TERM ;^`. With no `SET TERM`
# directive present, this reduces to a plain quote-aware top-level `;` split.
split_top_level_statements <- function(sql) {
  statements <- list()
  term <- ";"
  lines <- strsplit(sql, "\n", fixed = TRUE)[[1]]
  if (length(lines) == 0) return(character())
  in_single <- FALSE
  in_double <- FALSE
  buf <- character(0)

  append_piece <- function(piece) {
    if (nzchar(piece)) {
      buf[[length(buf) + 1]] <<- piece
    }
  }

  flush <- function() {
    chunk <- trimws(paste(buf, collapse = ""))
    buf <<- character(0)
    if (!nzchar(chunk)) return(invisible(NULL))
    new_term <- chunk_set_term(chunk)
    if (!is.null(new_term)) {
      term <<- new_term
      return(invisible(NULL))
    }
    statements[[length(statements) + 1]] <<- chunk
    invisible(NULL)
  }

  for (line_index in seq_along(lines)) {
    line <- lines[[line_index]]
    n <- nchar(line, type = "chars", allowNA = FALSE, keepNA = FALSE)
    i <- 1
    segment_start <- 1
    while (i <= n) {
      ch <- substr(line, i, i)
      if (!in_single && !in_double && ch == "-" && i + 1 <= n && substr(line, i + 1, i + 1) == "-") {
        append_piece(substr(line, segment_start, n))
        segment_start <- n + 1
        i <- n + 1
        break
      }
      if (ch == "'" && !in_double) {
        in_single <- !in_single
        i <- i + 1
        next
      }
      if (ch == '"' && !in_single) {
        in_double <- !in_double
        i <- i + 1
        next
      }
      term_len <- nchar(term, type = "chars", allowNA = FALSE, keepNA = FALSE)
      if (!in_single && !in_double && term_len > 0 &&
          i + term_len - 1 <= n &&
          identical(substr(line, i, i + term_len - 1), term)) {
        if (i > segment_start) {
          append_piece(substr(line, segment_start, i - 1))
        }
        flush()
        i <- i + term_len
        segment_start <- i
        next
      }
      i <- i + 1
    }
    if (segment_start <= n) {
      append_piece(substr(line, segment_start, n))
    }
    if (line_index < length(lines) && length(buf) > 0) {
      append_piece("\n")
    }
  }
  flush()
  vapply(statements, identity, character(1))
}

sb_normalize <- function(sql, params = NULL) {
  if (is.null(params) || length(params) == 0) {
    return(list(sql = sql, params = list()))
  }
  if (!is.null(names(params)) && any(names(params) != "")) {
    return(rewrite_named(sql, params))
  }
  if (grepl("\\?", sql, fixed = FALSE)) {
    return(rewrite_positional(sql, params))
  }
  list(sql = sql, params = params)
}

has_named_params <- function(sql) {
  in_string <- FALSE
  chars <- strsplit(sql, "", fixed = TRUE)[[1]]
  for (i in seq_len(length(chars) - 1)) {
    ch <- chars[i]
    if (ch == "'") {
      in_string <- !in_string
      next
    }
    if (!in_string && (ch == ":" || ch == "@")) {
      next_ch <- chars[i + 1]
      if (grepl("[A-Za-z_]", next_ch)) return(TRUE)
    }
  }
  FALSE
}

rewrite_named <- function(sql, params) {
  if (!has_named_params(sql)) stop("named parameters provided but query has no named placeholders")
  lookup <- list()
  for (name in names(params)) {
    key <- sub("^[@:]", "", name)
    lookup[[key]] <- params[[name]]
  }
  out <- ""
  ordered <- list()
  in_string <- FALSE
  chars <- strsplit(sql, "", fixed = TRUE)[[1]]
  i <- 1
  while (i <= length(chars)) {
    ch <- chars[i]
    if (ch == "'") {
      in_string <- !in_string
      out <- paste0(out, ch)
      i <- i + 1
      next
    }
    if (!in_string && (ch == ":" || ch == "@") && i + 1 <= length(chars) && grepl("[A-Za-z_]", chars[i + 1])) {
      j <- i + 1
      while (j <= length(chars) && grepl("[A-Za-z0-9_]", chars[j])) {
        j <- j + 1
      }
      name <- paste(chars[(i + 1):(j - 1)], collapse = "")
      if (is.null(lookup[[name]])) stop(paste0("missing named parameter: ", name))
      ordered[[length(ordered) + 1]] <- lookup[[name]]
      out <- paste0(out, "$", length(ordered))
      i <- j
      next
    }
    out <- paste0(out, ch)
    i <- i + 1
  }
  list(sql = out, params = ordered)
}

rewrite_positional <- function(sql, params) {
  out <- ""
  ordered <- list()
  in_string <- FALSE
  chars <- strsplit(sql, "", fixed = TRUE)[[1]]
  i <- 1
  idx <- 1
  while (i <= length(chars)) {
    ch <- chars[i]
    if (ch == "'") {
      in_string <- !in_string
      out <- paste0(out, ch)
      i <- i + 1
      next
    }
    if (!in_string && ch == "?") {
      if (idx > length(params)) stop("not enough parameters")
      ordered[[length(ordered) + 1]] <- params[[idx]]
      idx <- idx + 1
      out <- paste0(out, "$", length(ordered))
      i <- i + 1
      next
    }
    out <- paste0(out, ch)
    i <- i + 1
  }
  if (idx <= length(params)) stop("too many parameters")
  list(sql = out, params = ordered)
}
