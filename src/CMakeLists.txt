add_subdirectory( ijedi )
add_subdirectory( mains )

# find cpplint.py provided by oops
find_file( CPPLINT_PY oops_cpplint.py HINTS ${oops_BINDIR} )

ecbuild_add_test( TARGET ijedi_coding_norms
                  TYPE SCRIPT
                  COMMAND ${CPPLINT_PY}
                  ARGS --quiet --recursive ${CMAKE_CURRENT_SOURCE_DIR}
                  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin )

if(BUILD_TESTING)
  # Tests are directory-scoped. Root/test policies cannot classify the
  # coding-norms test registered here; apply the same policy in its owner.
  include(${PROJECT_SOURCE_DIR}/test/ProtectTestSchedule.cmake)
  ijedi_protect_test_schedule()
endif()
