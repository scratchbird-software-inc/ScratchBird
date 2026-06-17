# Element Catalog

Status: public specification baseline
Search key: `INSTALLER-CATALOG`

Components are defined once, in a platform-agnostic **element catalog**. Every
installer (Windows, macOS, Linux) and the mobile dependency packages are
*frontends* that consume this catalog and render it in the platform's native
idiom. The catalog also generates the installable-component documentation, the
two-stage selection menu (06), and the silent-install answer-file schema.

## Element schema

Each element answers *what* it is, *why* it exists, and *who* would want it, plus
enough metadata for dependency resolution and platform-native installation.

```yaml
- id: driver.odbc                       # stable identifier
  name: ScratchBird ODBC Driver
  what:  ODBC 3.x driver (DSN and DSN-less) speaking the SB wire protocol.
  why:   Lets ODBC tools connect with no native bindings.
  layer: driver                         # engine|runtime|listener|parser|manager|driver|tool|ai|docs|examples|headers
  audiences: [single-driver, app-dev]   # WHO sees it as an option
  optionality: optional                 # required|recommended|optional
  depends_on: [runtime.shared-client]
  conflicts_with: []
  install_modes: []                     # engine.* uses [embedded, service]
  payload: [shared_lib]
  approx_size_mb: 2
  post_install: [register_odbc_driver]
  verify: "isql -v smoke connect"
  platforms:                            # availability is first-class
    windows: { state: supported,  arch: [x64, arm64], artifact: dll,   register: registry-DSN }
    linux:   { state: supported,  arch: [x86_64, arm64], artifact: so, register: odbcinst.ini }
    macos:   { state: supported,  arch: [universal],  artifact: dylib, register: iODBC }
    android: { state: unsupported }
    ios:     { state: unsupported }
```

Two fields carry the user-facing model: **`audiences`** (which target groups would
see this element as an option) and **`platforms[].state`** (`supported` /
`planned` / `unsupported`). Encoding `planned` keeps future platforms present in
the catalog and documentation without offering an artifact that does not exist
yet, so no platform is ever locked out of the model.

## Element inventory (seed)

Grouped by category. A full installation is on the order of 70–100 elements.

| Category | Elements | Notes |
| --- | --- | --- |
| Runtime core | `runtime.shared-client` | the wire/protocol client core; every native driver depends on it; placed once and shared |
| Engine tiers | `engine.server`, `engine.listener`, `engine.manager` | `engine.server` is one-per-database; `engine.listener` is a single binary configured per instance (05); `engine.manager` is the native-SBsql authority/broker |
| Parsers | `parser.sbsql` + 25+ `parser.<dialect>` | each dialect is an individually selectable wire-protocol front-end feeding a listener's pool; an "all parsers" metapackage exists |
| Drivers / adapters / tools | 34+ `driver.<lang>`, adapter, and tool elements | language drivers (each a thin binding over `runtime.shared-client`), adapters, and CLIs such as `tool.sb_isql`, `tool.sb_admin` |
| AI layer | `ai.<module>` | retrieval/integration modules; kept modular and independently versioned so the popular standard can be swapped without touching engine elements |
| Documentation | `docs.developer`, `docs.reference`, … | optional, prunable |
| Examples | `examples.*` | optional sample schemas and code |
| Headers | `headers.engine` | for the embedder audience |

`parser.*` elements are the operationally meaningful expression of compatibility:
installing a dialect's parser makes it available to configure a listener for that
dialect (05). The listener binary itself is a single element regardless of how
many dialects are installed.

## Audiences (target groups)

Personas are tag selections over the inventory; the installer presents one as the
"overall target group," then an editable menu (06). The same persona may run the
engine in different modes (embedded vs service) and under different policies (07).

| Persona | Typical element set |
| --- | --- |
| App developer (SDK) | engine (embedded) + dev docs + chosen drivers + headers + examples + CLIs |
| Operator / DBA | manager + per-database server + listeners + admin CLI; no docs/examples |
| Single-driver consumer | `runtime.shared-client` + one driver |
| Embedder | embeddable engine lib + headers only |
| Evaluator | engine + one CLI + sample database |

## Metapackages

To keep a ~100-element catalog navigable, the catalog defines metapackages that
express "all of category X" and per-persona bundles (for example a developer SDK
bundle, an operator/server bundle, an all-parsers bundle, an all-drivers bundle).
On Linux these map to native metapackages; on Windows/macOS they map to default
feature selections in the bundle/distribution package (06). Dependency resolution
(shared-client, engine) auto-includes prerequisites.
