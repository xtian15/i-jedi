cmake_minimum_required(VERSION 3.20)
project(ijedi_atlas_topology_focus LANGUAGES CXX)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
endif()
find_package(hic REQUIRED)
find_package(atlas REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
# Focused build of the same production adapter and the normal regression main;
# the complete I-JEDI target also includes these sources.
get_filename_component(IJEDI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(ijedi_mpas_atlas_topology
  "${IJEDI_ROOT}/src/ijedi/Geometry/mpas/MpasAtlasGeometry.cc"
  "${IJEDI_ROOT}/src/ijedi/Interpolation/MpasAtlasPointOperator.cc"
  "${IJEDI_ROOT}/src/ijedi/Interpolation/AtlasMeshIntegrity.cc"
  "${IJEDI_ROOT}/src/ijedi/Interpolation/AtlasCellMeanEndpoint.cc"
  "${IJEDI_ROOT}/src/ijedi/Interpolation/MpasAtlasConservativeOperator.cc"
  "${IJEDI_ROOT}/test/mains/ijediMpasAtlasTopology.cc")
target_compile_features(ijedi_mpas_atlas_topology PRIVATE cxx_std_17)
target_include_directories(ijedi_mpas_atlas_topology PRIVATE "${IJEDI_ROOT}/src")
target_link_libraries(ijedi_mpas_atlas_topology PRIVATE atlas nlohmann_json::nlohmann_json OpenSSL::Crypto)
