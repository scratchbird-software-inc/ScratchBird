# ScratchBird Metabase Driver

This adapter targets ScratchBird through the ScratchBird JDBC driver. The
Metabase layer stays intentionally thin and now aligns its connection-property
surface with the supported JDBC runtime behavior instead of carrying a reduced
or adapter-specific subset.

## Documentation

- Getting started: `docs/getting-started/metabase.md`
- API reference: `docs/api-reference/metabase.md`

## Contents

- `metabase-plugin.yaml` plugin manifest
- `deps.edn` Clojure deps (includes the ScratchBird JDBC driver)
- `src/metabase/driver/scratchbird.clj` adapter namespace
- `src/metabase/driver/scratchbird_support.clj` pure helper/config namespace

## Notes

- Update the JDBC dependency version to match your release.
- Metabase expects the driver to be packaged as a single JAR and placed in
  `MB_PLUGINS_DIR`.
- Connection properties now expose the full JDBC `sslmode` set, optional
  `currentSchema`, manager-proxy ingress fields, and the staged auth/bootstrap,
  workload-identity, proxy-assertion, and dormant-reattach fields admitted by
  the JDBC lane.
- If no `currentSchema` is supplied, the adapter preserves the server-side
  default schema policy, which falls back to `users.public`.
- The `can-connect?` implementation assumes Metabase's current
  `sql-jdbc.conn/can-connect-with-details?` helper. Adjust if the API changes.

## Minimal Build Notes (tools.build)

This is a minimal example using `tools.build` to produce a single JAR.

**1) Create `build.clj`:**
```clojure
(ns build
  (:require [clojure.tools.build.api :as b]))

(def lib 'scratchbird/metabase-driver)
(def version "0.1.0")
(def class-dir "target/classes")
(def basis (b/create-basis {:project "deps.edn"}))

(defn clean [_]
  (b/delete {:path "target"}))

(defn jar [_]
  (clean nil)
  (b/copy-dir {:src-dirs ["src" "resources"] :target-dir class-dir})
  (b/jar {:class-dir class-dir
          :jar-file (format "target/scratchbird-metabase-driver-%s.jar" version)}))
```

**2) Build the JAR:**
```bash
clj -T:build jar
```

## Sample Plugin JAR Layout

The final JAR should contain:
```
scratchbird-metabase-driver.jar
├── metabase-plugin.yaml
└── metabase/driver/scratchbird.clj
```

Place the JAR into `MB_PLUGINS_DIR` and restart Metabase.
