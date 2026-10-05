# Copyright (C) 2026 IC Weather LLC
# One source authority for the bundle and every MPAS test consumer.
# This contribution remains local-only until an approved accessible pin exists.
set(_ijedi_atlas_commit 463944947b5ac17ea4bdcc8da1004a30f4602920)
if(DEFINED IJEDI_MPAS_ATLAS_COMPILER_IDENTITY AND
   NOT IJEDI_MPAS_ATLAS_COMPILER_IDENTITY STREQUAL _ijedi_atlas_commit)
  message(FATAL_ERROR "MPAS Atlas compiler identity differs from the dependency pin")
endif()
set(IJEDI_MPAS_ATLAS_COMPILER_IDENTITY "${_ijedi_atlas_commit}")
unset(_ijedi_atlas_commit)

# Public OOPS headers require two exported templates and installed-layout test
# includes. This local prerequisite preserves upstream notices and equations.
set(_ijedi_oops_commit 670c6a82a35c446f9fe300f3637877ea5250d34a)
if(DEFINED IJEDI_MPAS_OOPS_SOURCE_IDENTITY AND
   NOT IJEDI_MPAS_OOPS_SOURCE_IDENTITY STREQUAL _ijedi_oops_commit)
  message(FATAL_ERROR "OOPS source identity differs from the dependency pin")
endif()
set(IJEDI_MPAS_OOPS_SOURCE_IDENTITY "${_ijedi_oops_commit}")
unset(_ijedi_oops_commit)
