;; Copyright (c) 2026 ScratchBird Software Inc.
;;
;; This Source Code Form is subject to the terms of the Mozilla Public
;; License, v. 2.0. If a copy of the MPL was not distributed with this
;; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;;
;; SPDX-License-Identifier: MPL-2.0

(ns metabase.driver.scratchbird-support
  (:require
    [clojure.string :as str]))

(def scratchbird-type->base-type
  {"BOOLEAN"                     :type/Boolean
   "SMALLINT"                    :type/Integer
   "INTEGER"                     :type/Integer
   "INT"                         :type/Integer
   "BIGINT"                      :type/BigInteger
   "INT8"                        :type/BigInteger
   "REAL"                        :type/Float
   "FLOAT"                       :type/Float
   "DOUBLE"                      :type/Float
   "DOUBLE PRECISION"            :type/Float
   "NUMERIC"                     :type/Decimal
   "DECIMAL"                     :type/Decimal
   "CHAR"                        :type/Text
   "CHARACTER"                   :type/Text
   "VARCHAR"                     :type/Text
   "CHARACTER VARYING"           :type/Text
   "TEXT"                        :type/Text
   "CLOB"                        :type/Text
   "DATE"                        :type/Date
   "TIME"                        :type/Time
   "TIME WITHOUT TIME ZONE"      :type/Time
   "TIME WITH TIME ZONE"         :type/TimeWithTZ
   "TIMESTAMP"                   :type/DateTime
   "TIMESTAMP WITHOUT TIME ZONE" :type/DateTime
   "TIMESTAMP WITH TIME ZONE"    :type/DateTimeWithTZ
   "TIMESTAMPTZ"                 :type/DateTimeWithTZ
   "UUID"                        :type/UUID
   "JSON"                        :type/JSON
   "JSONB"                       :type/JSON
   "BLOB"                        :type/*
   "BYTEA"                       :type/*
   "ARRAY"                       :type/Array
   "VECTOR"                      :type/Array
   "GEOMETRY"                    :type/*
   "GEOGRAPHY"                   :type/*
   "COMPOSITE"                   :type/Structured
   "RANGE"                       :type/Structured
   "RECORD"                      :type/Structured
   "ROW"                         :type/Structured
   "VARIANT"                     :type/JSON
   "INET"                        :type/IPAddress
   "CIDR"                        :type/IPAddress
   "MACADDR"                     :type/Text
   "BIT"                         :type/*
   "BIT VARYING"                 :type/*
   "XML"                         :type/Text
   "INTERVAL"                    :type/*
   "MONEY"                       :type/Decimal
   "TSVECTOR"                    :type/Text
   "TSQUERY"                     :type/Text
   "UNKNOWN"                     :type/*
   "SERIAL"                      :type/Integer
   "BIGSERIAL"                   :type/BigInteger})

(def scratchbird-feature-support
  {:foreign-keys                    true
   :schemas                         true
   :basic-aggregations              true
   :standard-deviation-aggregations true
   :expression-aggregations         true
   :percentile-aggregations         true
   :expressions                     true
   :expressions/today               true
   :expressions/datetime            true
   :expressions/date                true
   :expressions/integer             true
   :expressions/float               true
   :expressions/text                true
   :split-part                      true
   :regex                           true
   :regex/lookaheads-and-lookbehinds false
   :collate                         true
   :window-functions/cumulative     true
   :window-functions/offset         true
   :metadata/key-constraints        true
   :describe-fields                 true
   :describe-indexes                true
   :table-privileges                false
   :nested-field-columns            false
   :uploads                         false
   :upload-with-auto-pk             false
   :connection/multiple-databases   false
   :uuid-type                       true
   :identifiers-with-spaces         true})

(def scratchbird-connection-properties
  [{:name "host" :display-name "Host" :type :string :default "localhost" :required true}
   {:name "port" :display-name "Port" :type :integer :default 3092 :required true}
   {:name "db" :display-name "Database" :type :string :required true}
   {:name "user" :display-name "Username" :type :string :required true}
   {:name "password" :display-name "Password" :type :password :required true}
   {:name "sslmode" :display-name "SSL Mode" :type :select
    :options [{:name "disable" :value "disable"}
              {:name "allow" :value "allow"}
              {:name "prefer" :value "prefer"}
              {:name "require" :value "require"}
              {:name "verify-ca" :value "verify-ca"}
              {:name "verify-full" :value "verify-full"}]
    :default "require"}
   {:name "sslrootcert" :display-name "CA Certificate" :type :string}
   {:name "sslcert" :display-name "Client Certificate" :type :string}
   {:name "sslkey" :display-name "Client Key" :type :string}
   {:name "sslpassword" :display-name "SSL Key Password" :type :password}
   {:name "role" :display-name "Role" :type :string}
   {:name "currentSchema" :display-name "Current Schema" :type :string
    :helper-text "Optional override. If omitted, ScratchBird uses the server-side user/role/group default schema, falling back to users.public."}
   {:name "search_path" :display-name "Search Path" :type :string
    :helper-text "Optional alias for Current Schema / Search Path style clients."}
   {:name "front_door_mode" :display-name "Ingress Mode" :type :select
    :options [{:name "direct" :value "direct"}
              {:name "manager_proxy" :value "manager_proxy"}]
    :default "direct"}
   {:name "manager_auth_token" :display-name "Manager Auth Token" :type :password}
   {:name "manager_username" :display-name "Manager Username" :type :string}
   {:name "manager_database" :display-name "Manager Database" :type :string}
   {:name "manager_connection_profile" :display-name "Manager Connection Profile" :type :string}
   {:name "manager_client_intent" :display-name "Manager Client Intent" :type :string}
   {:name "manager_client_flags" :display-name "Manager Client Flags" :type :string}
   {:name "manager_auth_fast_path" :display-name "Manager Auth Fast Path" :type :boolean :default false}
   {:name "auth_token" :display-name "Auth Token" :type :password}
   {:name "auth_method_id" :display-name "Auth Method Id" :type :string
    :helper-text "Must use the scratchbird.auth.* namespace when provided."}
   {:name "auth_method_payload" :display-name "Auth Method Payload" :type :password}
   {:name "auth_payload_json" :display-name "Auth Payload JSON" :type :string}
   {:name "auth_payload_b64" :display-name "Auth Payload Base64" :type :password}
   {:name "auth_provider_profile" :display-name "Auth Provider Profile" :type :string}
   {:name "auth_required_methods" :display-name "Required Auth Methods" :type :string}
   {:name "auth_forbidden_methods" :display-name "Forbidden Auth Methods" :type :string}
   {:name "auth_require_channel_binding" :display-name "Require Channel Binding" :type :boolean :default false}
   {:name "workload_identity_token" :display-name "Workload Identity Token" :type :password}
   {:name "proxy_principal_assertion" :display-name "Proxy Principal Assertion" :type :password}
   {:name "dormant_id" :display-name "Dormant Id" :type :string}
   {:name "dormant_reattach_token" :display-name "Dormant Reattach Token" :type :password}
   {:name "application_name" :display-name "Application Name" :type :string :default "metabase"}
   {:name "connectTimeout" :display-name "Connect Timeout (seconds)" :type :integer}
   {:name "socketTimeout" :display-name "Socket Timeout (seconds)" :type :integer}
   {:name "fetch_size" :display-name "Fetch Size" :type :integer}
   {:name "connect_client_flags" :display-name "Connect Client Flags" :type :string}
   {:name "binaryTransfer" :display-name "Binary Transfer" :type :boolean :default true
    :helper-text "ScratchBird uses binary-first parameter binding. Set false only for JDBC-compatibility paths that explicitly require text transfer."}])

(def ^:private allowed-sslmodes
  #{"disable" "allow" "prefer" "require" "verify-ca" "verify-full"})

(def ^:private allowed-front-door-modes
  #{"direct" "manager_proxy"})

(defn resolved-detail
  [details & aliases]
  (some (fn [alias]
          (let [keyword-alias (if (keyword? alias) alias (keyword alias))]
            (or (get details alias)
                (get details keyword-alias))))
        aliases))

(defn normalize-db-type
  [database-type]
  (-> (or database-type "")
      str/trim
      str/upper-case
      (str/replace #"\s+" " ")
      (str/replace #"\(.*\)" "")
      str/trim))

(defn normalize-sslmode
  [details]
  (let [ssl-value (resolved-detail details :sslmode :ssl)
        sslmode (-> (or ssl-value "require")
                    str
                    str/lower-case
                    (#(cond
                        (#{"true" "1" "yes" "on"} %) "require"
                        (#{"false" "0" "no" "off"} %) "disable"
                        :else %)))]
    (when-not (contains? allowed-sslmodes sslmode)
      (throw (ex-info "ScratchBird requires a valid sslmode value."
                      {:sslmode sslmode
                       :allowed allowed-sslmodes})))
    sslmode))

(defn resolved-current-schema
  [details]
  (resolved-detail details :currentSchema :current_schema :searchPath :search_path))

(defn resolved-front-door-mode
  [details]
  (let [mode (some-> (or (resolved-detail details :front_door_mode :frontDoorMode) "direct")
                     str
                     str/lower-case)]
    (when-not (contains? allowed-front-door-modes mode)
      (throw (ex-info "ScratchBird requires front_door_mode to be direct or manager_proxy."
                      {:front_door_mode mode
                       :allowed allowed-front-door-modes})))
    mode))

(defn resolved-auth-method-id
  [details]
  (let [auth-method-id (resolved-detail details :auth_method_id :authMethodId)]
    (when (some? auth-method-id)
      (let [value (str auth-method-id)]
        (when-not (str/starts-with? value "scratchbird.auth.")
          (throw (ex-info "ScratchBird auth_method_id must start with scratchbird.auth."
                          {:auth_method_id value})))
        value))))

(defn ->jdbc-properties
  [details]
  (let [sslmode (normalize-sslmode details)
        binary-transfer (if (some? (resolved-detail details :binaryTransfer :binary_transfer))
                          (resolved-detail details :binaryTransfer :binary_transfer)
                          true)
        current-schema (resolved-current-schema details)
        front-door-mode (resolved-front-door-mode details)
        auth-method-id (resolved-auth-method-id details)]
    (cond-> {"sslmode" sslmode
             "application_name" (or (resolved-detail details :application_name :applicationName) "metabase")
             "binaryTransfer" (str (boolean binary-transfer))}
      (:sslrootcert details) (assoc "sslrootcert" (:sslrootcert details))
      (:sslcert details) (assoc "sslcert" (:sslcert details))
      (:sslkey details) (assoc "sslkey" (:sslkey details))
      (some? (resolved-detail details :connectTimeout :connect_timeout)) (assoc "connectTimeout" (str (resolved-detail details :connectTimeout :connect_timeout)))
      (some? (resolved-detail details :socketTimeout :socket_timeout)) (assoc "socketTimeout" (str (resolved-detail details :socketTimeout :socket_timeout)))
      (some? (resolved-detail details :fetch_size :fetchSize)) (assoc "fetch_size" (str (resolved-detail details :fetch_size :fetchSize)))
      (some? (resolved-detail details :connect_client_flags :connectClientFlags)) (assoc "connect_client_flags" (resolved-detail details :connect_client_flags :connectClientFlags))
      (some? (resolved-detail details :sslpassword)) (assoc "sslpassword" (resolved-detail details :sslpassword))
      (some? (resolved-detail details :role)) (assoc "role" (resolved-detail details :role))
      (some? current-schema) (assoc "currentSchema" current-schema)
      true (assoc "front_door_mode" front-door-mode)
      (some? (resolved-detail details :manager_auth_token :managerAuthToken)) (assoc "manager_auth_token" (resolved-detail details :manager_auth_token :managerAuthToken))
      (some? (resolved-detail details :manager_username :managerUsername)) (assoc "manager_username" (resolved-detail details :manager_username :managerUsername))
      (some? (resolved-detail details :manager_database :managerDatabase)) (assoc "manager_database" (resolved-detail details :manager_database :managerDatabase))
      (some? (resolved-detail details :manager_connection_profile :managerConnectionProfile)) (assoc "manager_connection_profile" (resolved-detail details :manager_connection_profile :managerConnectionProfile))
      (some? (resolved-detail details :manager_client_intent :managerClientIntent)) (assoc "manager_client_intent" (resolved-detail details :manager_client_intent :managerClientIntent))
      (some? (resolved-detail details :manager_client_flags :managerClientFlags)) (assoc "manager_client_flags" (resolved-detail details :manager_client_flags :managerClientFlags))
      (some? (resolved-detail details :manager_auth_fast_path :managerAuthFastPath)) (assoc "manager_auth_fast_path" (str (boolean (resolved-detail details :manager_auth_fast_path :managerAuthFastPath))))
      (some? (resolved-detail details :auth_token :authToken)) (assoc "auth_token" (resolved-detail details :auth_token :authToken))
      (some? auth-method-id) (assoc "auth_method_id" auth-method-id)
      (some? (resolved-detail details :auth_method_payload :authMethodPayload)) (assoc "auth_method_payload" (resolved-detail details :auth_method_payload :authMethodPayload))
      (some? (resolved-detail details :auth_payload_json :authPayloadJson)) (assoc "auth_payload_json" (resolved-detail details :auth_payload_json :authPayloadJson))
      (some? (resolved-detail details :auth_payload_b64 :authPayloadB64)) (assoc "auth_payload_b64" (resolved-detail details :auth_payload_b64 :authPayloadB64))
      (some? (resolved-detail details :auth_provider_profile :authProviderProfile)) (assoc "auth_provider_profile" (resolved-detail details :auth_provider_profile :authProviderProfile))
      (some? (resolved-detail details :auth_required_methods :authRequiredMethods)) (assoc "auth_required_methods" (resolved-detail details :auth_required_methods :authRequiredMethods))
      (some? (resolved-detail details :auth_forbidden_methods :authForbiddenMethods)) (assoc "auth_forbidden_methods" (resolved-detail details :auth_forbidden_methods :authForbiddenMethods))
      (some? (resolved-detail details :auth_require_channel_binding :authRequireChannelBinding)) (assoc "auth_require_channel_binding" (str (boolean (resolved-detail details :auth_require_channel_binding :authRequireChannelBinding))))
      (some? (resolved-detail details :workload_identity_token :workloadIdentityToken)) (assoc "workload_identity_token" (resolved-detail details :workload_identity_token :workloadIdentityToken))
      (some? (resolved-detail details :proxy_principal_assertion :proxyPrincipalAssertion)) (assoc "proxy_principal_assertion" (resolved-detail details :proxy_principal_assertion :proxyPrincipalAssertion))
      (some? (resolved-detail details :dormant_id :dormantId)) (assoc "dormant_id" (resolved-detail details :dormant_id :dormantId))
      (some? (resolved-detail details :dormant_reattach_token :dormantReattachToken)) (assoc "dormant_reattach_token" (resolved-detail details :dormant_reattach_token :dormantReattachToken)))))
