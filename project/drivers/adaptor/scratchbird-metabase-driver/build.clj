;; Copyright (c) 2026 ScratchBird Software Inc.
;;
;; This Source Code Form is subject to the terms of the Mozilla Public
;; License, v. 2.0. If a copy of the MPL was not distributed with this
;; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;;
;; SPDX-License-Identifier: MPL-2.0

(ns build
  (:require
    [clojure.string :as str]
    [clojure.tools.build.api :as b]))

(def lib 'scratchbird/metabase-driver)
(def version "0.1.0")
(def class-dir "target/classes")
(def basis (b/create-basis {:project "deps.edn"}))

(defn validate-package-contract! []
  (let [contract (slurp "package_contract.json")
        needles ["\"component_id\": \"adaptor:scratchbird-metabase-driver\""
                 "\"mode\": \"delegates_to_jdbc\""
                 "\"target_component\": \"driver:jdbc\""
                 "\"server_revalidation_required\": true"
                 "\"transaction_authority\": \"mga_engine\""
                 "\"this_artifact_includes_dbeaver\": false"]]
    (doseq [needle needles]
      (when-not (str/includes? contract needle)
        (throw (ex-info "package_contract.json missing required Metabase package posture"
                        {:needle needle}))))))

(defn clean [_]
  (b/delete {:path "target"}))

(defn jar [_]
  (validate-package-contract!)
  (clean nil)
  (b/copy-dir {:src-dirs ["src" "resources"] :target-dir class-dir})
  (spit (str class-dir "/metabase-plugin.yaml") (slurp "metabase-plugin.yaml"))
  (spit (str class-dir "/package_contract.json") (slurp "package_contract.json"))
  (b/jar {:class-dir class-dir
          :jar-file (format "target/scratchbird-metabase-driver-%s.jar" version)}))
