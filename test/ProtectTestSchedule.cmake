# Shared by production registration and the executable scheduler regression.
# Default to one expensive numerical test; only explicit cheap checks overlap.
function(ijedi_protect_test_schedule)
  get_property(_ijedi_tests DIRECTORY PROPERTY TESTS)
  # An unfamiliar test stays heavy even if its name sounds like a cheap check.
  # These owned entry points only inspect already-produced results/source.
  set(_ijedi_light_tests
      ijedi_mpas_oops_vs_direct
      ijedi_mpas_oops_generation_zero_compare
      ijedi_mpas_oops_detached_clone_compare
      ijedi_mpas_oops_fresh_restore_compare
      ijedi_mpas_oops_getvalues_tlad_vs_direct
      ijedi_mpas_analysis_bridge_vs_direct
      ijedi_mpas_transforms_nonlinear
      ijedi_mpas_typed_oracle_negative_controls
      ijedi_mpas_analysis_oracle_negative_controls
      ijedi_mpas_oops_tlad_oracle_negative_controls
      ijedi_mpas_release_gate_negative_controls
      ijedi_mpas_reference_correction_controls
      ijedi_mpas_retired_geometry_absent)
  foreach(_test IN LISTS _ijedi_tests)
    if(_test IN_LIST _ijedi_light_tests OR _test MATCHES "coding_norms")
      set_property(TEST "${_test}" APPEND PROPERTY LABELS ijedi_schedule_light)
    elseif(_test MATCHES "^ijedi_mpas_atlas_(point|conservative)_operator$")
      # These include wall-time/scaling measurements. Exclude even light tests.
      set_tests_properties("${_test}" PROPERTIES RUN_SERIAL TRUE)
      set_property(TEST "${_test}" APPEND PROPERTY LABELS ijedi_schedule_isolated)
    else()
      set_property(TEST "${_test}" APPEND PROPERTY RESOURCE_LOCK ijedi_heavy)
      set_property(TEST "${_test}" APPEND PROPERTY LABELS ijedi_schedule_heavy)
    endif()
    get_property(_dependencies TEST "${_test}" PROPERTY DEPENDS)
    foreach(_producer IN LISTS _dependencies)
      if(NOT _producer IN_LIST _ijedi_tests)
        message(FATAL_ERROR "Unregistered I-JEDI producer: ${_producer}")
      endif()
      # DEPENDS only orders tests. Fixtures also require producer success,
      # and auto-select the producer when a consumer is selected in isolation.
      set_property(TEST "${_producer}" APPEND PROPERTY
                   FIXTURES_SETUP "ijedi_result_${_producer}")
      set_property(TEST "${_test}" APPEND PROPERTY
                   FIXTURES_REQUIRED "ijedi_result_${_producer}")
    endforeach()
  endforeach()
endfunction()
