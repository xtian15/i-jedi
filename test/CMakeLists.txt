# Build the test executable
# -------------------------
add_subdirectory(mains)

set(IJEDI_MPAS_P0_CASE_DIR "" CACHE PATH
    "Directory containing the receipt-matching x1.10242 P0 input triplet")
set(IJEDI_MPAS_P0_WHEEL "" CACHE FILEPATH "Exact MPAS-PyTorch P0 wheel")
set(IJEDI_MPAS_P0_PYTHON "" CACHE FILEPATH "Python executable with that wheel installed")
set(IJEDI_MPAS_P0_DIRECT_ORACLE "" CACHE FILEPATH
    "Independent direct-Python two-step JSON oracle")
set(IJEDI_MPAS_RUNTIME_RECEIPT "${PROJECT_SOURCE_DIR}/tools/mpas_bridge/p0_runtime_receipt.json"
    CACHE FILEPATH "Exact declared MPAS/Python runtime receipt")
set(IJEDI_MPAS_GEOMETRY_CASES "" CACHE FILEPATH
    "Three ordered model-owned native geometry cases for required release scaling")
set(IJEDI_MPAS_GEOMETRY_LOCATIONS "" CACHE PATH
    "Directory containing the fixed 256 and 10000 target scaling corpora")
set(IJEDI_MPAS_RELEASE_STAGE "" CACHE STRING
    "Mandatory MPAS release registration: empty for legacy use, pr1 or pr2 for qualification")
set_property(CACHE IJEDI_MPAS_RELEASE_STAGE PROPERTY STRINGS "" pr1 pr2)
include(${CMAKE_CURRENT_SOURCE_DIR}/BindMpasInputs.cmake)
if(IJEDI_MPAS_P0_CASE_DIR AND IJEDI_MPAS_P0_WHEEL AND IJEDI_MPAS_P0_PYTHON AND
   IJEDI_MPAS_P0_DIRECT_ORACLE)
  set(IJEDI_MPAS_P0_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_two_step.json")
  set(IJEDI_MPAS_P0_CHECKPOINT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_step1_state.bin")
  set(IJEDI_MPAS_P0_RESTORE_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_fresh_restore.json")
  set(IJEDI_MPAS_P0_BUNDLE_ATTACK_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_bundle_attack.json")
  set(IJEDI_MPAS_P0_GENERATION_ZERO_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_generation_zero.json")
  set(IJEDI_MPAS_P0_DETACHED_CLONE_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_detached_clone.json")
  set(IJEDI_MPAS_P0_EXECUTION_MODE producer)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-oops-step.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-step.yaml @ONLY)
  set(IJEDI_MPAS_P0_OUTPUT ${IJEDI_MPAS_P0_BUNDLE_ATTACK_OUTPUT})
  set(IJEDI_MPAS_P0_EXECUTION_MODE bundle-attack)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-oops-step.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-bundle-attack.yaml @ONLY)
  set(IJEDI_MPAS_P0_OUTPUT ${IJEDI_MPAS_P0_GENERATION_ZERO_OUTPUT})
  set(IJEDI_MPAS_P0_EXECUTION_MODE generation-zero)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-oops-step.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-generation-zero.yaml @ONLY)
  set(IJEDI_MPAS_P0_OUTPUT ${IJEDI_MPAS_P0_DETACHED_CLONE_OUTPUT})
  set(IJEDI_MPAS_P0_EXECUTION_MODE detached-clone)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-oops-step.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-detached-clone.yaml @ONLY)
  set(IJEDI_MPAS_P0_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_oops_two_step.json")
  set(IJEDI_MPAS_P0_EXECUTION_MODE producer)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-oops-restore.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-restore.yaml @ONLY)
  ecbuild_add_test(TARGET ijedi_mpas_api_contracts
                   MPI 1
                   OMP 1
                   COMMAND ${IJEDI_MPAS_P0_PYTHON}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_mpas_api_contracts.py
                     --executable $<TARGET_FILE:ijedi_mpas_api_contracts.x>
                     --traversal-executable $<TARGET_FILE:ijedi_mpas_traversal_costs.x>
                     --configuration ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-step.yaml
                     --case ${IJEDI_MPAS_P0_CASE_DIR}
                     --runtime-receipt ${IJEDI_MPAS_RUNTIME_RECEIPT}
                     --wheel ${IJEDI_MPAS_P0_WHEEL}
                     --atlas-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                     --output-directory ${CMAKE_CURRENT_BINARY_DIR}/data_generated/api_contracts)
  set_tests_properties(ijedi_mpas_api_contracts PROPERTIES RUN_SERIAL TRUE)
  ecbuild_add_test(TARGET ijedi_mpas_oops_bundle_attack
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_step.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-bundle-attack.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_generation_zero
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_step.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-generation-zero.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_detached_clone
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_step.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-detached-clone.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_two_step
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_step.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-step.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_vs_direct
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mpas_oops_manifest.py
                        --oops ${IJEDI_MPAS_P0_OUTPUT}
                        --direct ${IJEDI_MPAS_P0_DIRECT_ORACLE}
                   TEST_DEPENDS ijedi_mpas_oops_two_step)
  ecbuild_add_test(TARGET ijedi_mpas_oops_generation_zero_compare
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mpas_step2_manifest.py
                        --candidate ${IJEDI_MPAS_P0_GENERATION_ZERO_OUTPUT}
                        --producer ${IJEDI_MPAS_P0_OUTPUT}
                   TEST_DEPENDS ijedi_mpas_oops_generation_zero
                                ijedi_mpas_oops_two_step)
  ecbuild_add_test(TARGET ijedi_mpas_oops_detached_clone_compare
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mpas_step2_manifest.py
                        --candidate ${IJEDI_MPAS_P0_DETACHED_CLONE_OUTPUT}
                        --producer ${IJEDI_MPAS_P0_OUTPUT}
                   TEST_DEPENDS ijedi_mpas_oops_detached_clone
                                ijedi_mpas_oops_two_step)
  ecbuild_add_test(TARGET ijedi_mpas_oops_fresh_restore
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_step.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-restore.yaml
                   TEST_DEPENDS ijedi_mpas_oops_two_step)
  ecbuild_add_test(TARGET ijedi_mpas_oops_fresh_restore_compare
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_fresh_process_restore.py
                        --producer ${IJEDI_MPAS_P0_OUTPUT}
                        --consumer ${IJEDI_MPAS_P0_RESTORE_OUTPUT}
                   TEST_DEPENDS ijedi_mpas_oops_two_step ijedi_mpas_oops_fresh_restore)
  ecbuild_add_test(TARGET ijedi_mpas_geometry_contracts
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_geometry_contract.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --direct-manifest ${IJEDI_MPAS_P0_DIRECT_ORACLE}
                        --output
                          ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_geometry_contract.json)
  foreach(_geometry_gate IN ITEMS topology cache-restore negative-controls)
    if(_geometry_gate STREQUAL "topology")
      set(_geometry_test ijedi_mpas_atlas_topology)
    elseif(_geometry_gate STREQUAL "cache-restore")
      set(_geometry_test ijedi_mpas_atlas_cache_restore)
    else()
      set(_geometry_test ijedi_mpas_geometry_negative_controls)
    endif()
  ecbuild_add_test(TARGET ${_geometry_test}
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_atlas_topology.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                        --gate ${_geometry_gate}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --executable $<TARGET_FILE:ijedi_mpas_atlas_topology.x>
                        --output ${CMAKE_CURRENT_BINARY_DIR}/data_generated/atlas_${_geometry_gate})
  endforeach()
  if(IJEDI_MPAS_GEOMETRY_CASES OR IJEDI_MPAS_GEOMETRY_LOCATIONS)
    if(NOT EXISTS "${IJEDI_MPAS_GEOMETRY_CASES}" OR
       NOT EXISTS "${IJEDI_MPAS_GEOMETRY_LOCATIONS}/locations_256.json" OR
       NOT EXISTS "${IJEDI_MPAS_GEOMETRY_LOCATIONS}/locations_10000.json")
      message(FATAL_ERROR "Native Atlas scaling requires its complete three-case/two-corpus inputs")
    endif()
    ecbuild_add_test(TARGET ijedi_mpas_atlas_point_operator
                     TYPE SCRIPT
                     COMMAND ${CMAKE_COMMAND}
                     ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                          ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_atlas_point_scaling.py
                          --wheel ${IJEDI_MPAS_P0_WHEEL}
                          --wheel-sha256
                            ${IJEDI_MPAS_P0_WHEEL_SHA256}
                          --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                          --cases ${IJEDI_MPAS_GEOMETRY_CASES}
                          --locations ${IJEDI_MPAS_GEOMETRY_LOCATIONS}
                          --executable $<TARGET_FILE:ijedi_mpas_atlas_point_scaling.x>
                          --output ${CMAKE_CURRENT_BINARY_DIR}/data_generated/atlas_point_scaling)
    ecbuild_add_test(TARGET ijedi_mpas_atlas_conservative_operator
                     TYPE SCRIPT
                     COMMAND ${CMAKE_COMMAND}
                     ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                          ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_atlas_conservative_scaling.py
                          --wheel ${IJEDI_MPAS_P0_WHEEL}
                          --wheel-sha256
                            ${IJEDI_MPAS_P0_WHEEL_SHA256}
                          --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                          --cases ${IJEDI_MPAS_GEOMETRY_CASES}
                          --executable $<TARGET_FILE:ijedi_mpas_atlas_topology.x>
                          --output ${CMAKE_CURRENT_BINARY_DIR}/data_generated/atlas_conservative_scaling)
    set_tests_properties(ijedi_mpas_atlas_point_operator
                         ijedi_mpas_atlas_conservative_operator PROPERTIES TIMEOUT 14400)
  endif()
  ecbuild_add_test(TARGET ijedi_mpas_retired_geometry_absent
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_retired_geometry_absent.py
                        --source ${PROJECT_SOURCE_DIR}
                        --python ${IJEDI_MPAS_P0_PYTHON}
                        --library $<TARGET_FILE:ijedi>)
  ecbuild_add_test(TARGET ijedi_mpas_serialization_contracts
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_serialization_contract.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --output
                          ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_serialization_contract.json)
  ecbuild_add_test(TARGET ijedi_mpas_oops_runtime_identity_attacks
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/attack_mpas_runtime_identity.py
                        --executable $<TARGET_FILE:ijedi_mpas_oops_step.x>
                        --config
                          ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-oops-step.yaml
                        --output
                          ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_runtime_identity_attacks.json)
else()
  message(STATUS
      "MPAS/OOPS real-grid tests disabled: set IJEDI_MPAS_P0_CASE_DIR, "
      "IJEDI_MPAS_P0_WHEEL, IJEDI_MPAS_P0_PYTHON and IJEDI_MPAS_P0_DIRECT_ORACLE")
endif()

set(IJEDI_MPAS_P2_LOCATIONS "" CACHE FILEPATH
    "Ordered global point corpus for the MPAS OOPS LocalInterpolator")
ecbuild_add_test(TARGET ijedi_mpas_release_gate_negative_controls
                 TYPE SCRIPT
                 COMMAND ${Python3_EXECUTABLE}
                 ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_release_gate.py)
ecbuild_add_test(TARGET ijedi_mpas_reference_correction_controls
                 TYPE SCRIPT
                 COMMAND ${Python3_EXECUTABLE}
                 ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_reference_corrections.py)
set(IJEDI_MPAS_CONTRACT_SOURCE_DIR "" CACHE PATH
    "Clean exact MPAS dependency checkout supplying unchanged registry tests only")
if(IJEDI_MPAS_P0_CASE_DIR AND IJEDI_MPAS_P0_WHEEL AND IJEDI_MPAS_P0_PYTHON AND
   IJEDI_MPAS_P2_LOCATIONS)
  set(IJEDI_MPAS_P2_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_typed_interpolation.bin")
  set(IJEDI_MPAS_P2_DIRECT_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_typed_interpolation_direct.bin")
  set(IJEDI_MPAS_P2_DIRECT_MANIFEST
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_typed_interpolation_direct.json")
  set(IJEDI_MPAS_P2_METRICS
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_typed_interpolation_metrics.json")
  set(IJEDI_MPAS_ANALYSIS_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_analysis_bridge.json")
  set(IJEDI_MPAS_ANALYSIS_DIRECT
      "${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_analysis_bridge_direct.json")
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "ownership")
  set(IJEDI_MPAS_STATE_PROBE_MODE "views")
  set(IJEDI_MPAS_PARTIAL_PROBE false)
  set(IJEDI_MPAS_NATIVE_SOCKET_PROBE false)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "lifetime")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-handle-lifetime.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "ownership")
  set(IJEDI_MPAS_STATE_PROBE_MODE "initial-clock")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-initial-clock.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "errors")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-handle-errors.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "generation")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-generation-limit.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "restart")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-runtime-restart.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "initial-boundary")
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-initial-boundary.yaml @ONLY)
  set(IJEDI_MPAS_HANDLE_PROBE_MODE "ownership")
  set(IJEDI_MPAS_STATE_PROBE_MODE "views")
  set(IJEDI_MPAS_PARTIAL_PROBE true)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-partial-probe.yaml @ONLY)
  set(IJEDI_MPAS_PARTIAL_PROBE false)
  set(IJEDI_MPAS_NATIVE_SOCKET_PROBE true)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_integration_mpas-typed-interpolation.yaml.in
      ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-native-p-socket.yaml @ONLY)
  set(IJEDI_MPAS_NATIVE_SOCKET_PROBE false)
  ecbuild_add_test(TARGET ijedi_mpas_variable_requests
                   MPI 1
                   OMP 1
                   COMMAND ${IJEDI_MPAS_P0_PYTHON}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_mpas_api_contracts.py
                     --executable $<TARGET_FILE:ijedi_mpas_variable_requests.x>
                     --configuration ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml
                     --case ${IJEDI_MPAS_P0_CASE_DIR}
                     --runtime-receipt ${IJEDI_MPAS_RUNTIME_RECEIPT}
                     --wheel ${IJEDI_MPAS_P0_WHEEL}
                     --atlas-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                     --output-directory ${CMAKE_CURRENT_BINARY_DIR}/data_generated/variable_requests)
  ecbuild_add_test(TARGET ijedi_mpas_initial_clock
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_state_atlas_views.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-initial-clock.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_handle_ownership
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_handle_lifetime
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-handle-lifetime.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_initial_boundary
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-initial-boundary.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_variable_negative_controls
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-handle-errors.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_generation_limit
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-generation-limit.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_runtime_restart_rejected
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_handle_ownership.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-runtime-restart.yaml)
  set_tests_properties(ijedi_mpas_runtime_restart_rejected PROPERTIES TIMEOUT 90)
  ecbuild_add_test(TARGET ijedi_mpas_typed_interpolation
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_typed_interpolation.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_typed_interpolation_direct
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/direct_typed_interpolation.py
                        --runtime-receipt ${IJEDI_MPAS_RUNTIME_RECEIPT}
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --atlas-cache ${IJEDI_MPAS_P2_OUTPUT}.atlas-cache.json
                        --atlas-metrics ${IJEDI_MPAS_P2_METRICS}
                        --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                        --locations ${IJEDI_MPAS_P2_LOCATIONS}
                        --output-values ${IJEDI_MPAS_P2_DIRECT_OUTPUT}
                        --output-manifest ${IJEDI_MPAS_P2_DIRECT_MANIFEST}
                   TEST_DEPENDS ijedi_mpas_typed_interpolation)
  ecbuild_add_test(TARGET ijedi_mpas_transforms_nonlinear
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_typed_interpolation.py
                        --oops ${IJEDI_MPAS_P2_OUTPUT}
                        --direct ${IJEDI_MPAS_P2_DIRECT_OUTPUT}
                        --manifest ${IJEDI_MPAS_P2_DIRECT_MANIFEST}
                   TEST_DEPENDS ijedi_mpas_typed_interpolation
                                ijedi_mpas_typed_interpolation_direct)
  ecbuild_add_test(TARGET ijedi_mpas_typed_oracle_negative_controls
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_compare_typed_interpolation.py)
  ecbuild_add_test(TARGET ijedi_mpas_analysis_oracle_negative_controls
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_compare_analysis_bridge.py)
  ecbuild_add_test(TARGET ijedi_mpas_analysis_bridge
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_analysis_bridge.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_increment_algebra
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_increment_algebra.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_partial_transforms
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_partial_transforms.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  set_tests_properties(ijedi_mpas_partial_transforms PROPERTIES TIMEOUT 3600)
  ecbuild_add_test(TARGET ijedi_mpas_control_native_tlad
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_partial_transforms.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-native-p-socket.yaml)
  set_tests_properties(ijedi_mpas_control_native_tlad PROPERTIES TIMEOUT 3600)
  ecbuild_add_test(TARGET ijedi_mpas_state_atlas_views
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_state_atlas_views.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_getvalues_tlad
                   MPI 1
                   OMP 1
                   COMMAND $<TARGET_FILE:ijedi_mpas_oops_getvalues_tlad.x>
                   ARGS ${CMAKE_CURRENT_BINARY_DIR}/tier0_integration_mpas-typed-interpolation.yaml)
  ecbuild_add_test(TARGET ijedi_mpas_oops_getvalues_tlad_direct
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/direct_oops_tlad.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --atlas-cache ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json.atlas-cache.json
                        --atlas-metrics ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json
                        --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                        --locations ${IJEDI_MPAS_P2_LOCATIONS}
                        --output ${IJEDI_MPAS_ANALYSIS_DIRECT}.oops-tlad.json
                   TEST_DEPENDS ijedi_mpas_oops_getvalues_tlad)
  ecbuild_add_test(TARGET ijedi_mpas_oops_getvalues_tlad_vs_direct
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_oops_tlad.py
                        --oops ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json
                        --direct ${IJEDI_MPAS_ANALYSIS_DIRECT}.oops-tlad.json
                   TEST_DEPENDS ijedi_mpas_oops_getvalues_tlad
                                ijedi_mpas_oops_getvalues_tlad_direct)
  ecbuild_add_test(TARGET ijedi_mpas_oops_tlad_oracle_negative_controls
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_compare_oops_tlad.py)
  ecbuild_add_test(TARGET ijedi_mpas_transforms_tlad
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_oops_native_composition_fd.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --atlas-cache ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json.atlas-cache.json
                        --atlas-metrics ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json
                        --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                        --locations ${IJEDI_MPAS_P2_LOCATIONS}
                        --oops ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-tlad.json
                        --output ${IJEDI_MPAS_ANALYSIS_OUTPUT}.oops-native-composition-fd.json
                   TEST_DEPENDS ijedi_mpas_oops_getvalues_tlad_vs_direct)
  # Resource limits for the complete three-state/four-seed response inventory;
  # numerical tolerances and mandatory cases remain unchanged.
  set_tests_properties(ijedi_mpas_oops_getvalues_tlad
                       ijedi_mpas_oops_getvalues_tlad_direct PROPERTIES TIMEOUT 3600)
  set_tests_properties(ijedi_mpas_transforms_tlad PROPERTIES TIMEOUT 14400)
  if(IJEDI_MPAS_CONTRACT_SOURCE_DIR)
    if(NOT EXISTS "${IJEDI_MPAS_CONTRACT_SOURCE_DIR}/dycore-torch/mpas-pytorch/tests/test_variable_registry.py")
      message(FATAL_ERROR "MPAS registry test authority is missing")
    endif()
    ecbuild_add_test(TARGET ijedi_mpas_variable_schema
                     TYPE SCRIPT
                     COMMAND ${CMAKE_COMMAND}
                     ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                          ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_variable_schema.py
                          --wheel ${IJEDI_MPAS_P0_WHEEL}
                          --wheel-sha256
                            ${IJEDI_MPAS_P0_WHEEL_SHA256}
                          --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                          --model-repo ${IJEDI_MPAS_CONTRACT_SOURCE_DIR}
                          --output ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_variable_schema.json)
  endif()
  ecbuild_add_test(TARGET ijedi_mpas_analysis_bridge_direct
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/direct_analysis_bridge.py
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --output ${IJEDI_MPAS_ANALYSIS_DIRECT})
  ecbuild_add_test(TARGET ijedi_mpas_analysis_bridge_vs_direct
                   TYPE SCRIPT
                   COMMAND ${Python3_EXECUTABLE}
                   ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_analysis_bridge.py
                        --embedded ${IJEDI_MPAS_ANALYSIS_OUTPUT}
                        --direct ${IJEDI_MPAS_ANALYSIS_DIRECT}
                   TEST_DEPENDS ijedi_mpas_analysis_bridge
                                ijedi_mpas_analysis_bridge_direct)
  foreach(_generation IN ITEMS 0 1 2)
    if(_generation EQUAL 1)
      set(_derivative_test ijedi_mpas_typed_derivatives_real)
    else()
      set(_derivative_test ijedi_mpas_typed_derivatives_real_generation_${_generation})
    endif()
  ecbuild_add_test(TARGET ${_derivative_test}
                   TYPE SCRIPT
                   COMMAND ${CMAKE_COMMAND}
                   ARGS -E env --unset=PYTHONPATH ${IJEDI_MPAS_P0_PYTHON}
                        ${PROJECT_SOURCE_DIR}/tools/mpas_bridge/validate_real_typed_derivatives.py
                        --init ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.init.nc
                        --grid ${IJEDI_MPAS_P0_CASE_DIR}/x1.10242.grid.nc
                        --namelist ${IJEDI_MPAS_P0_CASE_DIR}/namelist.atmosphere
                        --wheel ${IJEDI_MPAS_P0_WHEEL}
                        --atlas-cache ${IJEDI_MPAS_P2_OUTPUT}.atlas-cache.json
                        --atlas-metrics ${IJEDI_MPAS_P2_METRICS}
                        --atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}
                        --locations ${IJEDI_MPAS_P2_LOCATIONS}
                        --state-generation ${_generation}
                        --wheel-sha256
                          ${IJEDI_MPAS_P0_WHEEL_SHA256}
                        --source-commit ${IJEDI_MPAS_P0_SOURCE_COMMIT}
                        --output
                          ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mpas_typed_derivatives_${_generation}.json
                   TEST_DEPENDS ijedi_mpas_typed_interpolation)
  endforeach()
else()
  message(STATUS
      "MPAS typed LocalInterpolator test disabled: set P0 inputs and MPAS P2 inputs")
endif()

# Link the directory 'data' to the test directory of the build
# ------------------------------------------------------------
file(CREATE_LINK
    "${CMAKE_CURRENT_SOURCE_DIR}/data"
    "${CMAKE_CURRENT_BINARY_DIR}/data"
    SYMBOLIC
)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/data_generated)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mom6)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/data_generated/obs)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/data_generated/fv3)

# Link the directory 'testref' to the test directory of the build
# ---------------------------------------------------------------
file(CREATE_LINK
    "${CMAKE_CURRENT_SOURCE_DIR}/testref"
    "${CMAKE_CURRENT_BINARY_DIR}/testref"
    SYMBOLIC
)

# Test data generation
ecbuild_add_test(TARGET  ijedi_setup_mom6_testdata
                 TYPE    SCRIPT
                 COMMAND ${CMAKE_COMMAND}
                 ARGS    -E env --unset=PYTHONPATH ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_mom6_testdata.py
                         ${CMAKE_CURRENT_SOURCE_DIR}/data/mom6 ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mom6)

ecbuild_add_test(TARGET  ijedi_setup_obs_testdata
                 TYPE    SCRIPT
                 COMMAND ${CMAKE_COMMAND}
                 ARGS    -E env --unset=PYTHONPATH ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_obs_testdata.py
                         ${CMAKE_CURRENT_SOURCE_DIR}/data/obs ${CMAKE_CURRENT_BINARY_DIR}/data_generated/obs)

ecbuild_add_test(TARGET ijedi_setup_fv3_testdata
                 TYPE SCRIPT
                 COMMAND ${CMAKE_COMMAND}
                 ARGS    -E env --unset=PYTHONPATH ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fv3_testdata.py
                         ${CMAKE_CURRENT_BINARY_DIR}/data_generated/fv3)

# Geometry tests
ecbuild_add_test( TARGET  ijedi_geometry_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometry ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-fv3.yaml)

ecbuild_add_test( TARGET  ijedi_geometry_atlas
                  MPI     2
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometry ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-atlas.yaml)

ecbuild_add_test( TARGET  ijedi_geometry_gsibec
                  MPI     2
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometry ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-gsibec.yaml)

ecbuild_add_test( TARGET  ijedi_geometry_mom6
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometry ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

# OOPS GeometryIterator interface tests
ecbuild_add_test( TARGET  ijedi_geometryiterator_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometryiterator ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-fv3.yaml)

ecbuild_add_test( TARGET  ijedi_geometryiterator_mom6
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometryiterator ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

ecbuild_add_test( TARGET  ijedi_geometryiterator_fv3_3diter
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometryiterator
                          ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-fv3-3diter.yaml)

ecbuild_add_test( TARGET  ijedi_geometryiterator_mom6_3diter
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    geometryiterator
                          ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-mom6-3diter.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

# MOM6 structured-grid check against SOCA reference gridspec
ecbuild_add_test(TARGET  ijedi_check_mom6_structured_grid
                 TYPE    SCRIPT
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mom6_structured_grid.py
                 ARGS    ${CMAKE_CURRENT_BINARY_DIR}/mom6_structured_grid.nc
                         ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mom6/soca_gridspec.72x35x25.nc
                 TEST_DEPENDS ijedi_geometry_mom6)

# Increment tests
ecbuild_add_test( TARGET  ijedi_increment_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    increment ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_increment-fv3.yaml)

# State tests
ecbuild_add_test( TARGET  ijedi_state_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    state ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_state-fv3.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata)

ecbuild_add_test( TARGET  ijedi_state_mom6
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    state ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_state-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_geometry_cache_mom6)

# Persistence model test. This is intentionally data-free: it proves that the
# native I-JEDI model is factory-registered, preserves a zero state, and advances
# valid time by exactly the configured number of steps.
ecbuild_add_test( TARGET  ijedi_model_persistence
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    model ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_unit_model-persistence.yaml)

# Identity TL/AD test. The OOPS interface suite checks forward and reverse valid
# times in addition to linearity and the adjoint dot-product identity.
ecbuild_add_test( TARGET  ijedi_linearmodel_identity
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_test.x>
                  ARGS    linearmodel
                          ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier0_unit_linearmodel-identity.yaml)

# H(x) tests
ecbuild_add_test( TARGET  ijedi_hofx3d_mom6
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    hofx3d ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_hofx3d-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_setup_obs_testdata)

ecbuild_add_test( TARGET  ijedi_hofx3d_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    hofx3d ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_hofx3d-fv3.yaml
                  TEST_DEPENDS ijedi_setup_obs_testdata)

ecbuild_add_test( TARGET  ijedi_hofx3d_coupled
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi_coupled.x>
                  ARGS    hofx3d ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_hofx3d-coupled.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_setup_obs_testdata)

# Geometry cache test
ecbuild_add_test( TARGET  ijedi_geometry_cache_mom6
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    geometry_cache ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_geometry_cache-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

# Variational tests (depends on diffusion operator being calibrated first)
ecbuild_add_test( TARGET  ijedi_diffusion_mom6
                  MPI     2
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    errortoolbox ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_diffusion-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

ecbuild_add_test( TARGET  ijedi_3dvar_mom6
                  MPI     2
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    var ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_3dvar-mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_setup_obs_testdata ijedi_diffusion_mom6 ijedi_geometry_cache_mom6)

# 3DVar comparison: FV3 increment with interpolation to gaussian (Variant A) vs
# native atlas/gaussian increment (Variant B).
ecbuild_add_test( TARGET  ijedi_3dvar_fv3_interp_gauss
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    var ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_3dvar-fv3_interp_gauss.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata)

ecbuild_add_test( TARGET  ijedi_3dvar_fv3_inc_gauss
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    var ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_3dvar-fv3_inc_gauss.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata)

# LETKF tests
ecbuild_add_test( TARGET  ijedi_letkf_observer_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    letkf ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_letkf_observer-fv3.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata)

ecbuild_add_test( TARGET  ijedi_letkf_solver_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    letkf ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_letkf_solver-fv3.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata ijedi_letkf_observer_fv3)

ecbuild_add_test( TARGET  ijedi_letkf_observer_fv3-restart
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    letkf ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_letkf_observer-fv3-restart.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata)

ecbuild_add_test( TARGET  ijedi_letkf_solver_fv3-restart
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    letkf ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_letkf_solver-fv3-restart.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata ijedi_setup_obs_testdata ijedi_letkf_observer_fv3-restart)

# Dirac tests
ecbuild_add_test( TARGET  ijedi_dirac_fv3_interp_gauss
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    errortoolbox ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_dirac-fv3_interp_gauss.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata)

# Convert state tests
ecbuild_add_test( TARGET  ijedi_convertstate_mom6_to_mom6
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    convertstate ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_convertstate-mom6_to_mom6.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata)

ecbuild_add_test( TARGET  ijedi_convertstate_mom6_to_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    convertstate ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_convertstate-mom6_to_fv3.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_setup_fv3_testdata)

ecbuild_add_test( TARGET  ijedi_convertstate_fv3_to_atlas
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    convertstate ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_convertstate-fv3_to_atlas.yaml
                  TEST_DEPENDS ijedi_setup_mom6_testdata ijedi_setup_fv3_testdata)

ecbuild_add_test( TARGET  ijedi_convertstate_fv3_to_fv3
                  MPI     6
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    convertstate ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_convertstate-fv3_to_fv3.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata)

ecbuild_add_test( TARGET  ijedi_convertstate_fv3-restart_to_fv3-restart
                  MPI     1
                  OMP     1
                  COMMAND $<TARGET_FILE:ijedi.x>
                  ARGS    convertstate ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_convertstate-fv3-restart_to_fv3-restart.yaml
                  TEST_DEPENDS ijedi_setup_fv3_testdata)

# Atlas MeshBuilder ownership and physical file addressing must remain distinct.
foreach(_ranks IN ITEMS 2 6)
  if(_ranks EQUAL 6)
    set(_mom6_contract_config tier1_unit_geometry-mom6.yaml)
  else()
    set(_mom6_contract_config tier1_integration_diffusion-mom6.yaml)
  endif()
  ecbuild_add_test(TARGET ijedi_mesh_builder_mom6_${_ranks}
    MPI ${_ranks} OMP 1
    COMMAND $<TARGET_FILE:ijedi_mesh_builder_contract.x>
    ARGS ${CMAKE_CURRENT_SOURCE_DIR}/testinput/${_mom6_contract_config}
         ${CMAKE_CURRENT_BINARY_DIR}/data_generated/mom6_contract_${_ranks}.nc
    TEST_DEPENDS ijedi_setup_mom6_testdata)
endforeach()
ecbuild_add_test(TARGET ijedi_mesh_builder_fv3_6
  MPI 6 OMP 1
  COMMAND $<TARGET_FILE:ijedi_mesh_builder_contract.x>
  ARGS ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_unit_geometry-fv3.yaml
       ${CMAKE_CURRENT_BINARY_DIR}/data_generated/fv3_contract_unused.nc)
ecbuild_add_test(TARGET ijedi_mesh_builder_negative_controls
  TYPE SCRIPT COMMAND ${Python3_EXECUTABLE}
  ARGS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mesh_builder_rejections.py
       --mpi ${MPIEXEC_EXECUTABLE} "--mpi-args=${MPI_ARGS}"
       --executable $<TARGET_FILE:ijedi_mesh_builder_contract.x>
       --config ${CMAKE_CURRENT_SOURCE_DIR}/testinput/tier1_integration_diffusion-mom6.yaml
       --output ${CMAKE_CURRENT_BINARY_DIR}/data_generated/unused_negative.nc
  TEST_DEPENDS ijedi_setup_mom6_testdata)

# Apply after all producers and consumers are registered. No test is removed.
include(${CMAKE_CURRENT_SOURCE_DIR}/ProtectTestSchedule.cmake)
ijedi_protect_test_schedule()
