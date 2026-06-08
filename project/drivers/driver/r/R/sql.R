# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

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
