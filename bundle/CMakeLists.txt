cmake_minimum_required(VERSION 3.23 FATAL_ERROR)

project(ijedi-bundle VERSION 0.0.1 LANGUAGES C CXX Fortran)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MpasDependencyPins.cmake)

# The exact CPU Atlas replay contract is an unfused binary64 reduction.
# GNU/Clang otherwise contract multiply-adds on ARM even at -O2. Apply the
# numerical setting to the dependency graph, not just the I-JEDI consumer.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
endif()

find_package(ecbuild 3.5 REQUIRED)
include(ecbuild_bundle)

ecbuild_bundle_initialize()

# ===============================================================================
# user options
# ===============================================================================
option(TEST_IODA "download and ioda-data repo needed for ioda ctests" OFF)
option(TEST_UFO "download and ufo-data repo needed for ufo ctests" OFF)
option(TEST_VADER "download and jedi-model-data repo needed for saber and vader ctests" OFF)

# ECMWF libraries
option("BUNDLE_SKIP_ECKIT" "Don't build eckit" "ON") # Skip eckit build unless user passes -DBUNDLE_SKIP_ECKIT=OFF
option("BUNDLE_SKIP_FCKIT" "Don't build fckit" "ON") # Skip fckit build unless user passes -DBUNDLE_SKIP_FCKIT=OFF
option("BUNDLE_SKIP_ATLAS" "Don't build atlas" "ON") # Skip atlas build unless user passes -DBUNDLE_SKIP_ATLAS=OFF

# turn off optional OOPS toy models
option(ENABLE_LORENZ95_MODEL "Build Lorenz95 toy model" OFF)
option(ENABLE_QG_MODEL "Build QG toy model" OFF)
option(ENABLE_OMP "Build the dependency graph with coherent OpenMP support" ON)

# Options to build JCSDA repositories
option(BUILD_JEDI "Build JEDI" ON)
ecbuild_bundle(PROJECT eckit GIT "https://github.com/ecmwf/eckit.git"
               TAG 3dcda7fd7ceacdeee7a04e3271554a99daa95668)
# This public upstream fix replaces libc strcmp's architecture-dependent
# magnitude with std::string comparison. Retain the unchanged ordering tests.

# Some ncurses installations split terminal symbols into libtinfo.  CMake's
# FindCurses module can report only libcurses/libform in that layout, leaving
# eckit_cmd with unresolved symbols that are detected only when it is loaded.
# Bind the provider explicitly when it exists; monolithic ncurses installs do
# not need this target-local addition.
find_library(IJEDI_TINFO_LIBRARY NAMES tinfo)
if(TARGET eckit_cmd AND IJEDI_TINFO_LIBRARY)
  target_link_libraries(eckit_cmd PRIVATE ${IJEDI_TINFO_LIBRARY})
endif()

ecbuild_bundle(PROJECT fckit GIT "https://github.com/ecmwf/fckit.git"
               TAG 9e93eae522beb9a7d93e13382c5e62af71a2e96a)

# The published image's ECTRANS crashes in spectral MPI distribution with
# GNU 13.3.0. Its supported optional-array compiler workaround passes the
# unchanged test. Build that exact dependency; do not inherit the image binary.
set(ECTRANS_HAVE_CONTIGUOUS_ISSUE ON CACHE BOOL
    "Use the validated ECTRANS optional-contiguous-array compiler workaround" FORCE)
ecbuild_bundle(PROJECT ectrans GIT "https://github.com/ecmwf-ifs/ectrans.git"
               TAG ed88c5aa886795319dce6fe46d0e7665dce26f9f)

# ecbuild passes its MPI launcher arguments directly to test executables.  The
# Atlas convenience wrapper expects those values through ATLAS_RUN_* variables
# instead, so using both produces commands beginning with ':OVERSUBSCRIBE'.
# Retain the one ecbuild/OpenMPI launcher used by the rest of this graph.
set(ENABLE_ATLAS_RUN OFF CACHE BOOL "Use the graph's external MPI launcher for Atlas tests" FORCE)
if(NOT BUNDLE_SKIP_ATLAS)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BindPocketfft.cmake)
endif()
ecbuild_bundle(PROJECT atlas GIT "https://github.com/ecmwf/atlas.git"
               TAG ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY})
# This Atlas contribution is local until publication is explicitly authorized.
# Local clean builds seed the dependency from the verified external Git bundle.
# Remote CI/review is blocked until this exact commit is accessible upstream
# or from an explicitly approved immutable fork; do not substitute develop.

# ===============================================================================
# optional test data (needed first since other bundles check for data)
# ===============================================================================
if(TEST_IODA)
  ecbuild_bundle(PROJECT ioda-data GIT "https://github.com/JCSDA-internal/ioda-data.git"
                 TAG 86b9275ef21a52e9990f48895477bf3951c54d5c)
endif()

if(TEST_UFO)
  ecbuild_bundle(PROJECT ufo-data GIT "https://github.com/JCSDA-internal/ufo-data.git"
                 TAG 636e43e4f528ff53da2736b85f24723f006d92f8)
endif()

if(TEST_VADER)
  ecbuild_bundle(PROJECT jedi-model-data GIT "https://github.com/JCSDA-internal/jedi-model-data.git" BRANCH develop UPDATE)
endif()

# ===============================================================================
# required repositories

# ===============================================================================
ecbuild_bundle(PROJECT jedicmake GIT "https://github.com/jcsda/jedi-cmake.git"
               TAG 40d521f9a2d796fcbc6234d77abceeffefb8eb7f RECURSIVE)
if (BUILD_JEDI)
  ecbuild_bundle(PROJECT oops GIT "https://github.com/JCSDA/oops.git"
                 TAG ${IJEDI_MPAS_OOPS_SOURCE_IDENTITY})
  # Local-only export prerequisite, seeded from the authenticated Git bundle.
  # Remote qualification needs an approved accessible immutable source.
  ecbuild_bundle(PROJECT vader GIT "https://github.com/JCSDA/vader.git"
                 TAG cb75e639ca09da1b132a777a81f6fe56209f64dd)
  ecbuild_bundle(PROJECT saber GIT "https://github.com/JCSDA/saber.git"
                 TAG 5f0c875f8fc19e6a9abc4c20cd04c38e7234436c)
  ecbuild_bundle(PROJECT ioda GIT "https://github.com/JCSDA/ioda.git"
                 TAG c744b03e83ccf9abae2d74e213556e5170f68547)
  ecbuild_bundle(PROJECT ufo GIT "https://github.com/JCSDA/ufo.git"
                 TAG edcd23aa29e1f52fb8373e5ff89f8af25e0bf51f)
  option(BUILD_FV3JEDI "Build fv3-jedi" OFF)
  if (BUILD_FV3JEDI)
    ecbuild_bundle( PROJECT fv3-jedi-lm    GIT "https://github.com/JCSDA/fv3-jedi-linearmodel.git"   UPDATE BRANCH develop )
    option(ENABLE_FV3_JEDI_DATA "Obtain fv3-jedi test data from fv3-jedi-data repository (vs tarball)" ON)
    ecbuild_bundle( PROJECT fv3-jedi-data  GIT "https://github.com/JCSDA-internal/fv3-jedi-data.git" UPDATE BRANCH develop )
    ecbuild_bundle( PROJECT fv3-jedi  GIT    "https://github.com/JCSDA/fv3-jedi.git"   UPDATE BRANCH develop )
  endif()
endif()
ecbuild_bundle(PROJECT ijedi SOURCE "../")

# ===============================================================================
# required repositories
# ===============================================================================
include(jedicmake/cmake/Functions/git_functions.cmake)

# ===============================================================================
ecbuild_bundle_finalize()
