# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

sb_tls_connect_native <- function(cfg) {
  root_cert <- if (is.null(cfg$sslrootcert)) "" else as.character(cfg$sslrootcert)
  cert_file <- if (is.null(cfg$sslcert)) "" else as.character(cfg$sslcert)
  key_file <- if (is.null(cfg$sslkey)) "" else as.character(cfg$sslkey)
  key_password <- if (is.null(cfg$sslpassword)) "" else as.character(cfg$sslpassword)

  .Call(
    C_sb_tls_connect,
    as.character(cfg$host),
    as.integer(cfg$port),
    tolower(as.character(cfg$sslmode)),
    root_cert,
    cert_file,
    key_file,
    key_password,
    as.integer(cfg$connect_timeout_ms),
    as.integer(cfg$socket_timeout_ms)
  )
}

sb_tls_write_native <- function(handle, data) {
  .Call(C_sb_tls_write, handle, data)
}

sb_tls_read_exact_native <- function(handle, n) {
  .Call(C_sb_tls_read_exact, handle, as.integer(n))
}

sb_tls_close_native <- function(handle) {
  invisible(.Call(C_sb_tls_close, handle))
}
