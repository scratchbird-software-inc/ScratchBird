# Toolchain And Dependency Policy

Search key: `DRIVER_TOOLCHAIN_AND_DEPENDENCY_POLICY`

## Required Toolchains

Every imported driver/adaptor/tool has a required toolchain row in
`DRIVER_SOURCE_INVENTORY.csv`. Development builds may allow CTest skip/waiver
behavior for missing non-critical toolchains by configuring with:

```bash
-DSB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS=ON
```

Release validation must configure:

```bash
-DSB_DRIVER_REQUIRE_ALL_TOOLCHAINS=ON
-DSB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS=OFF
```

## Dependency Cache Rules

Package-manager downloads and dependency trees must not be written under
`project/drivers/`.

Required cache roots:

- Cargo: `build/drivers/_deps/cargo`
- Go: `build/drivers/_deps/go`
- npm: `build/drivers/_deps/npm`
- Maven/Gradle: `build/drivers/_deps/jvm`
- Python: `build/drivers/_deps/python`
- Composer: `build/drivers/_deps/composer`
- Ruby/Bundler: `build/drivers/_deps/ruby`
- R: `build/drivers/_deps/r`
- SwiftPM: `build/drivers/_deps/swift`
- Dart pub: `build/drivers/_deps/dart`
- Mix/Hex: `build/drivers/_deps/elixir`

Lockfiles are source authority and may remain in source. Downloaded dependency
trees are generated artifacts and must be under `build/`.
