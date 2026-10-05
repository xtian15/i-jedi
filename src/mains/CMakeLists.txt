ecbuild_add_executable(TARGET ijedi.x
    SOURCES ijedi.cc
    LIBS ijedi ufo ioda saber)

ecbuild_add_executable(TARGET ijedi_coupled.x
    SOURCES ijedi_coupled.cc
    LIBS ijedi ufo ioda saber)

# The OOPS application templates make these two otherwise small translation
# units unusually expensive to compile with DWARF enabled.  This opt-in changes
# only debug-information emission for the drivers; linked libraries, runtime
# optimization, and test criteria are unchanged.
if(IJEDI_LOW_MEMORY_MAIN_COMPILE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ijedi.x PRIVATE -g0)
    target_compile_options(ijedi_coupled.x PRIVATE -g0)
  else()
    message(FATAL_ERROR
      "IJEDI_LOW_MEMORY_MAIN_COMPILE is unsupported for ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()
