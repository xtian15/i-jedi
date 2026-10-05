# ###############################################################################
# i-jedi - Interface to JEDI
# ###############################################################################

cmake_minimum_required(VERSION 3.23 FATAL_ERROR)

# Qualification binds the actual compiled sources, not just a CMake package
# name or a separately supplied clean Git checkout.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Record the actual compilation inputs for release qualification" FORCE)

project(ijedi VERSION 0.0.1 LANGUAGES C CXX Fortran)

# Also bind standalone I-JEDI builds. A preinstalled Atlas still requires
# separate exact-artifact qualification; consumer flags cannot change its code.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
endif()

option(OPENMP "Build i-jedi with OpenMP support" ON)
option(IJEDI_LOW_MEMORY_MAIN_COMPILE
       "Compile the template-heavy driver translation units without debug information"
       OFF)

find_package(ecbuild 3.5 REQUIRED)
include(ecbuild_system NO_POLICY_SCOPE)
ecbuild_declare_project()

include(GNUInstallDirs)

# ###############################################################################
# Dependencies
# ###############################################################################
find_package(NetCDF REQUIRED COMPONENTS C CXX Fortran)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 2.13.5 EXACT CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json REQUIRED CONFIG)

# find_package( gsl-lite REQUIRED HINTS $ENV{gsl_list_DIR})
find_package(eckit 1.28.3 EXACT REQUIRED)
find_package(fckit 0.13.2 EXACT REQUIRED)
find_package(atlas 0.46.0 EXACT REQUIRED)


find_package(vader 1.10.0 EXACT REQUIRED)
find_package(oops 1.13.0 EXACT REQUIRED)
find_package(saber 1.13.0 EXACT REQUIRED)
find_package(ioda 2.12.0 EXACT REQUIRED)
find_package(ufo 1.13.0 EXACT REQUIRED)
if(OPENMP)
  find_package(OpenMP)
endif()

# ###############################################################################
# Model Specific Dependencies
# ###############################################################################

# FMS for FV3 and MOM6
find_package(FMS 2023.04 COMPONENTS R4 R8)
add_library(fms ALIAS FMS::fms_r8)

# ###############################################################################
# Sources
# ###############################################################################
add_subdirectory(src)

# ###############################################################################
# CTest Directory
# ###############################################################################
if(BUILD_TESTING)
  add_subdirectory(test)
endif()

if(ECBUILD_INSTALL_FORTRAN_MODULES)
  install(DIRECTORY ${CMAKE_Fortran_MODULE_DIRECTORY}/${CMAKE_CFG_INTDIR} DESTINATION ${INSTALL_INCLUDE_DIR})
endif()

# ###############################################################################
# Finalise configuration
# ###############################################################################

# prepares a tar.gz of the sources and/or binaries
ecbuild_install_project(NAME ${PROJECT_NAME})

# print the summary of the configuration
ecbuild_print_summary()

if(BUILD_TESTING)
  # ecbuild finalisation may register root-level checks after test/ is visited.
  include(${PROJECT_SOURCE_DIR}/test/ProtectTestSchedule.cmake)
  ijedi_protect_test_schedule()
endif()
