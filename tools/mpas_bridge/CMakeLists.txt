cmake_minimum_required(VERSION 3.23 FATAL_ERROR)
project(ijedi_mpas_bridge_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 3.11 EXACT REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 2.13.5 EXACT CONFIG REQUIRED)

add_executable(ijedi_mpas_bridge_spike
  main.cc
  ../../src/ijedi/Python/PythonRuntime.cc
  ../../src/ijedi/Python/MpasPythonAdapter.cc
)
target_include_directories(ijedi_mpas_bridge_spike PRIVATE
  ../../src
)
target_link_libraries(ijedi_mpas_bridge_spike PRIVATE Python3::Python pybind11::embed)
# This executable has no exported ABI. Match pybind11's hidden internal types
# rather than suppressing the compiler's visibility warning.
set_target_properties(ijedi_mpas_bridge_spike PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ijedi_mpas_bridge_spike PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
