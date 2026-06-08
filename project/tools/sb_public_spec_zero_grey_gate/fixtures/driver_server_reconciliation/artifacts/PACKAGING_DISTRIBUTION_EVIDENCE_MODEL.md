# Packaging Distribution Evidence Model

Search key: `DRIVER_SERVER_PACKAGING_DISTRIBUTION`.

## Purpose

Make driver, adapter, and tool distribution evidence part of release readiness
instead of a manual afterthought.

## Required Evidence Per Lane

Every driver, adapter, and tool row-status manifest MUST include a
`package_evidence` mapping. The CTest packaging gate rejects the lane if any of
these fields are absent or empty:

| Field | Requirement |
| --- | --- |
| `package_manifest_ref` | Path-plus-row reference to the package manifest that identifies the component. The default project manifest is `project/drivers/DriverPackageManifest.csv` with the component id. |
| `package_name` | Ecosystem package/distribution name. |
| `package_type` | Ecosystem package type, for example wheel, gem, npm package, Maven artifact, NuGet package, CMake install tree, CLI archive, or adapter plugin. |
| `version_source` | File or build variable that owns the package version. |
| `compatibility_range` | Supported ScratchBird server/protocol and host-framework version range. |
| `build_command` | Reproducible package build command. |
| `package_output_dir` | Build-tree or package-output directory that receives generated artifacts. |
| `install_smoke_ref` | Command, script, or CTest name that installs the produced package into a clean target and performs a basic attach/connect smoke. |
| `runtime_dependency_ref` | Runtime dependency list or lockfile evidence. |
| `sbom_ref` | SBOM artifact reference, generator command, or explicit ecosystem citation when SBOM is not supported. |
| `signing_ref` | Signing artifact or command, or explicit non-signing citation for ecosystems that do not support signing yet. |
| `license_ref` | License and notice-file evidence included in the package. |
| `clean_uninstall_ref` | Clean uninstall, overwrite install, or plugin removal smoke evidence. |
| `build_tree_isolation_ref` | Evidence that build/test/package output is confined to the build tree or declared package output directory. |
| `ctest_label` | CTest label or lane test name that proves install smoke and package sanity. This must cite the lane conformance label or `DSR-033`. |

Aliases accepted by the enforcement script are limited to spelling variants for
the same evidence concepts, such as `packaging_evidence`,
`install_smoke_command`, `sbom_status`, `signing_status`, `license_status`,
`clean_uninstall`, and `artifact_isolation_ref`.

## Package Manifest

`project/drivers/DriverPackageManifest.csv` is the required component package
manifest for P3. Each row-status manifest must point back to the matching
`component_id` row and the gate also verifies that the component source path and
CTest conformance profile from the package manifest are populated.

Lane-specific package manifests such as `package.json`, `pyproject.toml`,
`pom.xml`, `build.gradle`, `Cargo.toml`, `Package.swift`, `DESCRIPTION`,
`composer.json`, `pubspec.yaml`, plugin descriptors, or `package_contract.json`
may be cited as additional evidence, but they do not replace the project-level
package manifest row.

## Install Smoke

Install smoke evidence must install the produced package or plugin from the
declared package output directory into a clean target. A source-tree import or
in-place package-manager test is not sufficient. The smoke must include a basic
ScratchBird attach/connect or adapter initialization check and must record the
CTest label or lane command that enforces it.

## SBOM, Signing, And License

SBOM, signing, license, and notice evidence are release blockers:

- `sbom_ref` cites the generated SBOM or the explicit ecosystem limitation.
- `signing_ref` cites the signature/checksum/provenance artifact or explicit
  non-signing citation.
- `license_ref` cites the included license and notice files.
- `runtime_dependency_ref` cites the lockfile, dependency manifest, or generated
  dependency inventory used to create the SBOM/license evidence.

## Clean Uninstall

Package evidence must prove that installation is reversible or safely
overwriteable. Driver libraries, CLI tools, and adapter plugins must cite a
clean-uninstall smoke, plugin removal smoke, or overwrite-install smoke that
leaves no source-tree build output behind.

## CTest Labels

P3 package evidence is enforced by CTest labels:

- `DSR-030` covers driver row-status manifests.
- `DSR-031` covers adapter row-status manifests.
- `DSR-032` covers driver/adaptor/tool CTest row-status integration.
- `DSR-033` covers package manifest, install smoke, SBOM, signing, license,
  clean uninstall, and build-tree isolation evidence.

## Build Artifact Rule

All package build, test, and generated files must stay under the build tree or
the lane package output directory. Source-tree generated files are release
blockers unless they are checked-in source artifacts listed by the execution_plan. The
packaging CTest gate rejects a `package_output_dir` that points inside the
component source tree.

## Closure Rule

`DSR-033` closes only when every claimed driver, adapter, and tool lane has a
row-status manifest with complete package evidence, install smoke, SBOM/signing
/license disposition, clean uninstall or overwrite-install evidence,
build-tree isolation, and a CTest label tied to the row-status manifest.
