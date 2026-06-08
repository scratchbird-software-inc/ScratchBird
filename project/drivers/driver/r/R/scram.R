# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

sb_scram_client <- function(username, algorithm = "sha256") {
  nonce <- openssl::base64_encode(openssl::rand_bytes(18))
  list(
    username = username,
    client_nonce = nonce,
    client_first_bare = "",
    server_signature = NULL,
    algorithm = algorithm
  )
}

raw_xor <- function(a, b) {
  as.raw(bitwXor(as.integer(a), as.integer(b)))
}

hmac_hash <- function(algorithm, key, message) {
  key_raw <- if (is.raw(key)) key else charToRaw(as.character(key))
  msg_raw <- if (is.raw(message)) message else charToRaw(as.character(message))
  if (identical(algorithm, "sha512")) {
    hash_fn <- openssl::sha512
    block_size <- 128L
  } else {
    hash_fn <- openssl::sha256
    block_size <- 64L
  }
  if (length(key_raw) > block_size) {
    key_raw <- hash_fn(key_raw)
  }
  if (length(key_raw) < block_size) {
    key_raw <- c(key_raw, as.raw(rep(0L, block_size - length(key_raw))))
  }
  o_key <- raw_xor(key_raw, as.raw(rep(0x5c, block_size)))
  i_key <- raw_xor(key_raw, as.raw(rep(0x36, block_size)))
  inner <- hash_fn(c(i_key, msg_raw))
  hash_fn(c(o_key, inner))
}

int_to_raw_be <- function(value) {
  con <- rawConnection(raw(), "wb")
  on.exit(close(con))
  writeBin(as.integer(value), con, size = 4, endian = "big")
  rawConnectionValue(con)
}

pbkdf2_hmac_hash <- function(password, salt, iterations, keylen, algorithm = "sha256") {
  hlen <- if (identical(algorithm, "sha512")) 64L else 32L
  blocks <- ceiling(keylen / hlen)
  out <- raw()
  for (i in seq_len(blocks)) {
    u <- hmac_hash(algorithm, password, c(salt, int_to_raw_be(i)))
    t <- u
    if (iterations > 1) {
      for (j in 2:iterations) {
        u <- hmac_hash(algorithm, password, u)
        t <- raw_xor(t, u)
      }
    }
    out <- c(out, t)
  }
  out[seq_len(keylen)]
}

sb_scram_client_first <- function(state) {
  escaped <- gsub("=", "=3D", gsub(",", "=2C", state$username, fixed = TRUE), fixed = TRUE)
  state$client_first_bare <- paste0("n=", escaped, ",r=", state$client_nonce)
  list(state = state, message = paste0("n,,", state$client_first_bare))
}

sb_scram_handle_server_first <- function(state, password, server_first) {
  attrs <- parse_scram_attrs(server_first)
  nonce <- attrs$r
  if (is.null(nonce) || !startsWith(nonce, state$client_nonce)) stop("SCRAM server nonce mismatch")
  salt_b64 <- attrs$s
  iter_str <- attrs$i
  if (is.null(salt_b64) || is.null(iter_str)) stop("SCRAM server-first missing fields")
  iterations <- as.integer(iter_str)
  salt <- openssl::base64_decode(salt_b64)
  algorithm <- if (!is.null(state$algorithm)) state$algorithm else "sha256"
  keylen <- if (identical(algorithm, "sha512")) 64L else 32L
  hash_fn <- if (identical(algorithm, "sha512")) openssl::sha512 else openssl::sha256
  salted <- pbkdf2_hmac_hash(password, salt, iterations, keylen = keylen, algorithm = algorithm)
  client_key <- hmac_hash(algorithm, salted, "Client Key")
  stored_key <- hash_fn(client_key)
  client_final_without_proof <- paste0("c=biws,r=", nonce)
  auth_message <- paste(state$client_first_bare, server_first, client_final_without_proof, sep = ",")
  client_signature <- hmac_hash(algorithm, stored_key, auth_message)
  client_proof <- raw_xor(client_key, client_signature)
  server_key <- hmac_hash(algorithm, salted, "Server Key")
  state$server_signature <- hmac_hash(algorithm, server_key, auth_message)
  proof_b64 <- openssl::base64_encode(as.raw(client_proof))
  list(state = state, message = paste0(client_final_without_proof, ",p=", proof_b64))
}

sb_scram_verify_server_final <- function(state, server_final) {
  attrs <- parse_scram_attrs(server_final)
  verifier <- attrs$v
  if (is.null(verifier)) stop("SCRAM server-final missing verifier")
  expected <- openssl::base64_encode(state$server_signature)
  if (verifier != expected) stop("SCRAM server signature mismatch")
  TRUE
}

parse_scram_attrs <- function(message) {
  attrs <- list()
  parts <- strsplit(message, ",", fixed = TRUE)[[1]]
  for (part in parts) {
    if (part == "") next
    kv <- strsplit(part, "=", fixed = TRUE)[[1]]
    if (length(kv) < 2) next
    attrs[[kv[1]]] <- kv[2]
  }
  attrs
}
