# Copyright (C) 2026 IC Weather LLC
# Retain this Atlas backend/test without a network-dependent feature switch.
set(IJEDI_POCKETFFT_SOURCE_DIR "" CACHE PATH "Exact retained PocketFFT source input")
if(NOT IJEDI_POCKETFFT_SOURCE_DIR OR NOT IS_DIRECTORY "${IJEDI_POCKETFFT_SOURCE_DIR}")
  message(FATAL_ERROR "Source-built Atlas requires the pinned PocketFFT source input")
endif()
set(_ijedi_artifact_manifest "${CMAKE_CURRENT_LIST_DIR}/MpasDependencyArtifacts.json")
file(READ "${_ijedi_artifact_manifest}" _ijedi_artifacts)
foreach(_ijedi_member IN ITEMS pocketfft_hdronly.h LICENSE.md)
  string(JSON _ijedi_expected GET "${_ijedi_artifacts}" pocketfft members "${_ijedi_member}")
  if(NOT EXISTS "${IJEDI_POCKETFFT_SOURCE_DIR}/${_ijedi_member}")
    message(FATAL_ERROR "Missing pinned PocketFFT member: ${_ijedi_member}")
  endif()
  file(SHA256 "${IJEDI_POCKETFFT_SOURCE_DIR}/${_ijedi_member}" _ijedi_actual)
  if(NOT _ijedi_actual STREQUAL _ijedi_expected)
    message(FATAL_ERROR "PocketFFT member differs from exact dependency receipt: ${_ijedi_member}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_ijedi_artifact_manifest}"
             "${IJEDI_POCKETFFT_SOURCE_DIR}/pocketfft_hdronly.h"
             "${IJEDI_POCKETFFT_SOURCE_DIR}/LICENSE.md")
set(ENABLE_POCKETFFT ON CACHE BOOL "Retain the authenticated Atlas FFT backend/tests" FORCE)
set(pocketfft_ROOT "${IJEDI_POCKETFFT_SOURCE_DIR}")
set(pocketfft_INCLUDE_DIR "${IJEDI_POCKETFFT_SOURCE_DIR}" CACHE PATH
    "Authenticated PocketFFT include input; never auto-download another copy" FORCE)
unset(_ijedi_artifact_manifest)
unset(_ijedi_artifacts)
unset(_ijedi_member)
unset(_ijedi_expected)
unset(_ijedi_actual)
