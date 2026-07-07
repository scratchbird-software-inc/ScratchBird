;; Copyright (c) 2026 ScratchBird Software Inc.
;;
;; This Source Code Form is subject to the terms of the Mozilla Public
;; License, v. 2.0. If a copy of the MPL was not distributed with this
;; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;;
;; SPDX-License-Identifier: MPL-2.0

(ns metabase.driver.scratchbird-support-test
  (:require
    [clojure.test :refer [deftest is testing]]
    [metabase.driver.scratchbird-support :as support]))

(deftest jdbc-properties-accept-current-jdbc-parity-options
  (let [props (support/->jdbc-properties {:sslmode "disable"
                                          :binaryTransfer false
                                          :currentSchema "tenant.analytics"
                                          :front_door_mode "manager_proxy"
                                          :manager_auth_token "secret-token"
                                          :manager_username "cluster-admin"
                                          :manager_database "control"
                                          :manager_connection_profile "ops"
                                          :manager_client_intent "bi"
                                          :manager_client_flags "analytics,readonly"
                                          :manager_auth_fast_path true
                                          :auth_token "opaque-token"
                                          :auth_method_id "scratchbird.auth.jwt_oidc"
                                          :auth_method_payload "payload"
                                          :auth_payload_json "{\"sub\":\"alice\"}"
                                          :auth_payload_b64 "ZXhhbXBsZQ=="
                                          :auth_provider_profile "entra"
                                          :auth_required_methods "TOKEN,SCRAM_SHA_512"
                                          :auth_forbidden_methods "MD5"
                                          :auth_require_channel_binding true
                                          :workload_identity_token "workload-token"
                                          :proxy_principal_assertion "proxy-assertion"
                                          :dormant_id "dormant-1"
                                          :dormant_reattach_token "reattach-token"
                                          :role "bi_reader"
                                          :application_name "metabase"
                                          :connectTimeout 7
                                          :socketTimeout 11
                                          :fetch_size 250
                                          :connect_client_flags "readonly,analytics"})]
    (is (= "disable" (get props "sslmode")))
    (is (= "false" (get props "binaryTransfer")))
    (is (= "tenant.analytics" (get props "currentSchema")))
    (is (= "manager_proxy" (get props "front_door_mode")))
    (is (= "secret-token" (get props "manager_auth_token")))
    (is (= "cluster-admin" (get props "manager_username")))
    (is (= "control" (get props "manager_database")))
    (is (= "ops" (get props "manager_connection_profile")))
    (is (= "bi" (get props "manager_client_intent")))
    (is (= "analytics,readonly" (get props "manager_client_flags")))
    (is (= "true" (get props "manager_auth_fast_path")))
    (is (= "opaque-token" (get props "auth_token")))
    (is (= "scratchbird.auth.jwt_oidc" (get props "auth_method_id")))
    (is (= "payload" (get props "auth_method_payload")))
    (is (= "{\"sub\":\"alice\"}" (get props "auth_payload_json")))
    (is (= "ZXhhbXBsZQ==" (get props "auth_payload_b64")))
    (is (= "entra" (get props "auth_provider_profile")))
    (is (= "TOKEN,SCRAM_SHA_512" (get props "auth_required_methods")))
    (is (= "MD5" (get props "auth_forbidden_methods")))
    (is (= "true" (get props "auth_require_channel_binding")))
    (is (= "workload-token" (get props "workload_identity_token")))
    (is (= "proxy-assertion" (get props "proxy_principal_assertion")))
    (is (= "dormant-1" (get props "dormant_id")))
    (is (= "reattach-token" (get props "dormant_reattach_token")))
    (is (= "bi_reader" (get props "role")))
    (is (= "7" (get props "connectTimeout")))
    (is (= "11" (get props "socketTimeout")))
    (is (= "250" (get props "fetch_size")))
    (is (= "readonly,analytics" (get props "connect_client_flags")))))

(deftest resolved-current-schema-honors-driver-aliases
  (is (= "tenant.ops" (support/resolved-current-schema {:currentSchema "tenant.ops"})))
  (is (= "tenant.ops" (support/resolved-current-schema {:searchPath "tenant.ops"})))
  (is (= "tenant.ops" (support/resolved-current-schema {:search_path "tenant.ops"}))))

(deftest staged-auth-bootstrap-resolution-honors-aliases-and-refuses-invalid-values
  (is (= "manager_proxy" (support/resolved-front-door-mode {:frontDoorMode "manager_proxy"})))
  (is (= "scratchbird.auth.jwt_oidc"
         (support/resolved-auth-method-id {:authMethodId "scratchbird.auth.jwt_oidc"})))
  (is (thrown-with-msg?
       clojure.lang.ExceptionInfo
       #"front_door_mode"
       (support/resolved-front-door-mode {:frontDoorMode "sidecar"})))
  (is (thrown-with-msg?
       clojure.lang.ExceptionInfo
       #"auth_method_id"
       (support/resolved-auth-method-id {:authMethodId "jwt_oidc"}))))

(deftest feature-support-reflects-jdbc-metadata-surface
  (testing "metadata and index discovery stay explicitly enabled"
    (is (true? (:schemas support/scratchbird-feature-support)))
    (is (true? (:metadata/key-constraints support/scratchbird-feature-support)))
    (is (true? (:describe-fields support/scratchbird-feature-support)))
    (is (true? (:describe-indexes support/scratchbird-feature-support))))
  (testing "non-implemented privilege/upload surfaces stay disabled"
    (is (false? (:table-privileges support/scratchbird-feature-support)))
    (is (false? (:uploads support/scratchbird-feature-support)))))

(deftest connection-property-schema-exposes-expanded-jdbc-surface
  (let [names (set (map :name support/scratchbird-connection-properties))
        sslmode-prop (first (filter #(= "sslmode" (:name %))
                                    support/scratchbird-connection-properties))]
    (is (contains? names "currentSchema"))
    (is (contains? names "search_path"))
    (is (contains? names "front_door_mode"))
    (is (contains? names "manager_auth_token"))
    (is (contains? names "auth_token"))
    (is (contains? names "auth_method_id"))
    (is (contains? names "auth_required_methods"))
    (is (contains? names "auth_forbidden_methods"))
    (is (contains? names "workload_identity_token"))
    (is (contains? names "proxy_principal_assertion"))
    (is (contains? names "dormant_id"))
    (is (contains? names "dormant_reattach_token"))
    (is (contains? names "fetch_size"))
    (is (contains? names "connect_client_flags"))
    (is (= #{"disable" "allow" "prefer" "require" "verify-ca" "verify-full"}
           (set (map :value (:options sslmode-prop)))))))
