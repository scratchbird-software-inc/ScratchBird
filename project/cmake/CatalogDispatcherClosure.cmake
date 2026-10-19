# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Completeness guard for the declared catalog dispatcher composition.
#
# src/core/catalog/CMakeLists.txt declares sb_core_catalog_dispatcher_objects:
# the catalog dispatcher plus one implementation per catalog family. Production
# consumes those objects through sb_core_catalog, and evidence fixtures that
# compile the dispatcher consume the same objects instead of restating the list
# as their own sources. See `ARCH-BUILD-COMPOSITION-DECLARED-ONCE` and
# `decisions/DR-BUILD-COMPOSITION-001`.
#
# A declaration still has to be kept true when a family is added. This guard
# makes that mechanical: every catalog translation unit that defines a family
# entry point (`IsCatalog<Family>Payload`) must be declared in the dispatcher
# composition. If one is not, configure fails naming the exact file to add,
# instead of the omission surfacing later as undefined references in whichever
# fixture happens to link the dispatcher.
#
# That is the failure this replaces: the family list was previously copied into
# each consumer, and drifted twice -- once when a fixture copied another
# target's sources before new families existed, and again when a refactor added
# nine families to the engine but not to the storage component closure.
#
# Note on counts: the dispatcher references more `IsCatalog*Payload` names than
# there are defining catalog sources. The remainder are satisfied inline in
# headers or by families owned by other libraries, and they link correctly. Do
# not widen the regex below to try to make those numbers agree; this scan
# intentionally finds only catalog sources carrying a definition.

function(sb_catalog_verify_dispatcher_closure_declared)
  foreach(_required sb_core_catalog sb_core_catalog_dispatcher_objects)
    if(NOT TARGET ${_required})
      message(FATAL_ERROR
        "sb_catalog_verify_dispatcher_closure_declared(): target ${_required} "
        "does not exist. Call this after both catalog targets are declared.")
    endif()
  endforeach()

  get_target_property(_dir sb_core_catalog SOURCE_DIR)
  get_target_property(_declared sb_core_catalog_dispatcher_objects SOURCES)

  # Normalise the declared composition to absolute paths.
  set(_declared_abs "")
  foreach(_src IN LISTS _declared)
    if(NOT IS_ABSOLUTE "${_src}")
      set(_src "${_dir}/${_src}")
    endif()
    get_filename_component(_src "${_src}" REALPATH)
    list(APPEND _declared_abs "${_src}")
  endforeach()

  # Candidate set: every catalog translation unit ON DISK.
  #
  # This deliberately globs the directory rather than reading the declared
  # lists. A file that is declared nowhere is precisely the case this guard
  # exists to catch, so deriving candidates from the declarations would make the
  # interesting failure invisible -- which it was, until the guard was
  # negative-tested by removing a family from the declaration and observing that
  # configure still passed.
  #
  # Globbing is appropriate here because this is a verification input, not a
  # definition of what gets built; the build composition remains the explicit
  # lists above. CMAKE_CONFIGURE_DEPENDS below re-runs the check when catalog
  # sources change.
  file(GLOB _candidates CONFIGURE_DEPENDS "${_dir}/*.cpp")
  list(APPEND _candidates ${_declared_abs})
  set(_normalised "")
  foreach(_src IN LISTS _candidates)
    get_filename_component(_src "${_src}" REALPATH)
    list(APPEND _normalised "${_src}")
  endforeach()
  set(_candidates ${_normalised})
  list(REMOVE_DUPLICATES _candidates)

  set(_definers "")
  set(_missing "")
  foreach(_src IN LISTS _candidates)
    if(NOT EXISTS "${_src}")
      continue()
    endif()
    file(STRINGS "${_src}" _hit
      REGEX "^(\\[\\[nodiscard\\]\\] )?bool[ \t]+IsCatalog[A-Za-z0-9]+Payload[ \t]*\\(" LIMIT_COUNT 1)
    if(_hit)
      list(APPEND _definers "${_src}")
      if(NOT _src IN_LIST _declared_abs)
        get_filename_component(_name "${_src}" NAME)
        list(APPEND _missing "${_name}")
      endif()
    endif()
  endforeach()

  if(NOT _definers)
    message(FATAL_ERROR
      "Catalog family entry-point scan matched nothing. The "
      "IsCatalog<Family>Payload definition form has probably changed; update "
      "the regex in cmake/CatalogDispatcherClosure.cmake. Refusing to report a "
      "complete dispatcher composition that was never actually checked.")
  endif()

  if(_missing)
    list(JOIN _missing "\n  " _missing_text)
    message(FATAL_ERROR
      "Catalog dispatcher composition is incomplete.\n\n"
      "These catalog sources define a family entry point but are not declared "
      "in sb_core_catalog_dispatcher_objects:\n  ${_missing_text}\n\n"
      "Add them to the add_library(sb_core_catalog_dispatcher_objects OBJECT ...) "
      "list in src/core/catalog/CMakeLists.txt. Every consumer that compiles the "
      "catalog dispatcher takes its implementations from that one declaration, "
      "so declaring it there is the only edit required.")
  endif()

  # Re-run this check when a catalog source changes, so adding a family entry
  # point to an existing file is caught without a manual reconfigure. The cost
  # is a reconfigure on catalog source edits.
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
    APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_candidates})

  list(LENGTH _definers _n)
  message(STATUS "catalog_dispatcher_composition_declared=complete families=${_n}")
endfunction()
