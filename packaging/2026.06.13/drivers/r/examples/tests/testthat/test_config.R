# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("parse URI config", {
  cfg <- sb_config("scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd")
  expect_equal(cfg$host, "localhost")
  expect_equal(cfg$port, 3092L)
  expect_equal(cfg$database, "mydb")
  expect_equal(cfg$user, "user")
  expect_equal(cfg$password, "pass")
  expect_equal(cfg$sslmode, "require")
  expect_equal(cfg$connect_timeout_ms, 3000L)
  expect_equal(cfg$application_name, "app")
  expect_false(cfg$binary_transfer)
  expect_equal(cfg$compression, "zstd")
})

test_that("parse key-value config", {
  cfg <- sb_config("Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7")
  expect_equal(cfg$host, "server")
  expect_equal(cfg$port, 4000L)
  expect_equal(cfg$database, "db")
  expect_equal(cfg$user, "me")
  expect_equal(cfg$password, "secret")
  expect_equal(cfg$connect_timeout_ms, 5000L)
  expect_equal(cfg$socket_timeout_ms, 7000L)
})

test_that("parse manager proxy params", {
  cfg <- sb_config("scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7")
  expect_equal(cfg$front_door_mode, "manager_proxy")
  expect_equal(cfg$manager_auth_token, "token")
  expect_equal(cfg$manager_client_flags, 7L)
})

test_that("parse staged auth bootstrap params", {
  cfg <- sb_config(
    paste0(
      "scratchbird://user:pass@localhost:3092/mydb?",
      "authtoken=token-123&connectclientflags=9&authmethodid=scratchbird.auth.scram_sha_512",
      "&authmethodpayload=opaque&authpayloadjson=%7B%22k%22%3A1%7D&authpayloadb64=YmluYXJ5",
      "&authproviderprofile=corp&authrequiredmethods=TOKEN&authforbiddenmethods=MD5",
      "&authrequirechannelbinding=true&workloadidentitytoken=widt&proxyprincipalassertion=proxy",
      "&dormantid=abc&dormantreattachtoken=def"
    )
  )

  expect_equal(cfg$auth_token, "token-123")
  expect_equal(cfg$connect_client_flags, 9L)
  expect_equal(cfg$auth_method_id, "scratchbird.auth.scram_sha_512")
  expect_equal(cfg$auth_method_payload, "opaque")
  expect_equal(cfg$auth_payload_json, '{"k":1}')
  expect_equal(cfg$auth_payload_b64, "YmluYXJ5")
  expect_equal(cfg$auth_provider_profile, "corp")
  expect_equal(cfg$auth_required_methods, "TOKEN")
  expect_equal(cfg$auth_forbidden_methods, "MD5")
  expect_true(cfg$auth_require_channel_binding)
  expect_equal(cfg$workload_identity_token, "widt")
  expect_equal(cfg$proxy_principal_assertion, "proxy")
  expect_equal(cfg$dormant_id, "abc")
  expect_equal(cfg$dormant_reattach_token, "def")
})

test_that("invalid front door mode errors", {
  expect_error(sb_config("scratchbird://localhost:3092/db?front_door_mode=invalid"), "front_door_mode must be direct or manager_proxy")
})
