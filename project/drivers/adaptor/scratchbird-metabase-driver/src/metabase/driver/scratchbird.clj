;; Copyright (c) 2026 ScratchBird Software Inc.
;;
;; This Source Code Form is subject to the terms of the Mozilla Public
;; License, v. 2.0. If a copy of the MPL was not distributed with this
;; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;;
;; SPDX-License-Identifier: MPL-2.0

(ns metabase.driver.scratchbird
  (:require
    [clojure.string :as str]
    [metabase.driver :as driver]
    [metabase.driver.common :as driver.common]
    [metabase.driver.scratchbird-support :as support]
    [metabase.driver.sql-jdbc :as sql-jdbc]
    [metabase.driver.sql-jdbc.connection :as sql-jdbc.conn]
    [metabase.driver.sql-jdbc.sync :as sql-jdbc.sync]
    [metabase.driver.sql.query-processor :as sql.qp]
    [metabase.util.honey-sql-2 :as h2x]))

(defmethod driver/display-name :scratchbird [_] "ScratchBird")

(defmethod driver/connection-properties :scratchbird
  [_]
  support/scratchbird-connection-properties)

(defmethod sql-jdbc.conn/connection-details->spec :scratchbird
  [_ details]
  (let [props (support/->jdbc-properties details)
        host (or (:host details) "localhost")
        port (let [raw-port (or (:port details) 3092)]
               (if (string? raw-port)
                 (Integer/parseInt raw-port)
                 raw-port))
        db   (:db details)]
    {:classname "com.scratchbird.jdbc.SBDriver"
     :subprotocol "scratchbird"
     :subname (format "//%s:%s/%s" host port db)
     :user (:user details)
     :password (:password details)
     :properties props}))

(defmethod driver/can-connect? :scratchbird
  [driver details]
  (sql-jdbc.conn/can-connect? driver details))

(defmethod driver/db-default-timezone :scratchbird
  [_ _]
  "UTC")

(defmethod driver/db-start-of-week :scratchbird
  [_ _]
  :monday)

(doseq [[feature supported?] support/scratchbird-feature-support]
  (defmethod driver/database-supports? [:scratchbird feature] [_ _ _] supported?))

(defmethod sql-jdbc.sync/database-type->base-type :scratchbird
  [driver database-type]
  (let [normalized (support/normalize-db-type database-type)
        mapped (get support/scratchbird-type->base-type normalized)]
    (or mapped
        (when (str/ends-with? normalized "[]") :type/Array)
        (sql-jdbc.sync/pattern-based-database-type->base-type driver database-type))))

(defmethod sql-jdbc.sync/column->semantic-type :scratchbird
  [driver database-type column-name]
  (let [normalized (support/normalize-db-type database-type)]
    (or (when (#{"JSON" "JSONB"} normalized) :type/SerializedJSON)
        ((get-method sql-jdbc.sync/column->semantic-type :sql-jdbc) driver database-type column-name))))

(defmethod sql.qp/current-datetime-honeysql-form :scratchbird
  [_driver]
  (h2x/current-datetime-honeysql-form :postgres))

(defn- extract-integer
  [unit expr]
  (h2x/->integer (h2x/extract unit (h2x/->pg-timestamp expr))))

(defn- date-trunc
  [unit expr]
  (let [expr' (h2x/->pg-timestamp expr)
        trunc [:date_trunc (h2x/literal unit) expr']]
    (h2x/with-database-type-info trunc (or (h2x/database-type expr') "timestamp"))))

(defmethod sql.qp/date [:scratchbird :default]          [_ _ expr] expr)
(defmethod sql.qp/date [:scratchbird :second-of-minute] [_ _ expr] (extract-integer :second expr))
(defmethod sql.qp/date [:scratchbird :minute]           [_ _ expr] (date-trunc :minute expr))
(defmethod sql.qp/date [:scratchbird :minute-of-hour]   [_ _ expr] (extract-integer :minute expr))
(defmethod sql.qp/date [:scratchbird :hour]             [_ _ expr] (date-trunc :hour expr))
(defmethod sql.qp/date [:scratchbird :hour-of-day]      [_ _ expr] (extract-integer :hour expr))
(defmethod sql.qp/date [:scratchbird :day-of-month]     [_ _ expr] (extract-integer :day expr))
(defmethod sql.qp/date [:scratchbird :day-of-year]      [_ _ expr] (extract-integer :doy expr))
(defmethod sql.qp/date [:scratchbird :month]            [_ _ expr] (date-trunc :month expr))
(defmethod sql.qp/date [:scratchbird :month-of-year]    [_ _ expr] (extract-integer :month expr))
(defmethod sql.qp/date [:scratchbird :quarter]          [_ _ expr] (date-trunc :quarter expr))
(defmethod sql.qp/date [:scratchbird :quarter-of-year]  [_ _ expr] (extract-integer :quarter expr))
(defmethod sql.qp/date [:scratchbird :year]             [_ _ expr] (date-trunc :year expr))
(defmethod sql.qp/date [:scratchbird :year-of-era]      [_ _ expr] (extract-integer :year expr))
(defmethod sql.qp/date [:scratchbird :week-of-year-iso] [_ _ expr] (extract-integer :week expr))

(defmethod sql.qp/date [:scratchbird :day-of-week]
  [driver _unit expr]
  (sql.qp/adjust-day-of-week driver
                             (h2x/+ (extract-integer :dow expr) 1)
                             (driver.common/start-of-week-offset-for-day :sunday)))

(defmethod sql.qp/date [:scratchbird :week]
  [_driver _unit expr]
  (sql.qp/adjust-start-of-week :scratchbird (partial date-trunc :week) expr))

(defmethod sql.qp/unix-timestamp->honeysql [:scratchbird :seconds]
  [_ _ expr]
  (h2x/with-database-type-info [:to_timestamp expr] "timestamptz"))

(defn init []
  (let [register (ns-resolve 'metabase.driver 'register!)]
    (if register
      (register :scratchbird :parent :sql-jdbc)
      (sql-jdbc/register-driver! :scratchbird))))
