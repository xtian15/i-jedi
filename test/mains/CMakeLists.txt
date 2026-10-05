ecbuild_add_executable(TARGET ijedi_mesh_builder_contract.x
    SOURCES ijediMeshBuilderContract.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_test.x
    SOURCES ijediTest.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_oops_step.x
    SOURCES ijediMpasOopsStep.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_api_contracts.x
    SOURCES ijediMpasApiContracts.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_traversal_costs.x
    SOURCES ijediMpasTraversalCosts.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_atlas_topology.x
    SOURCES ijediMpasAtlasTopology.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_atlas_point_scaling.x
    SOURCES ijediMpasAtlasPointScaling.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_atlas_execution_costs.x
    SOURCES ijediMpasAtlasExecutionCosts.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_typed_interpolation.x
    SOURCES ijediMpasTypedInterpolation.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_variable_requests.x
    SOURCES ijediMpasVariableRequests.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_analysis_bridge.x
    SOURCES ijediMpasAnalysisBridge.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_increment_algebra.x
    SOURCES ijediMpasIncrementAlgebra.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_oops_getvalues_tlad.x
    SOURCES ijediMpasOopsGetValuesTlad.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_state_atlas_views.x
    SOURCES ijediMpasStateAtlasViews.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_handle_ownership.x
    SOURCES ijediMpasHandleOwnership.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_transfer_probe.x
    SOURCES ijediMpasTransferProbe.cc
    LIBS ijedi)

ecbuild_add_executable(TARGET ijedi_mpas_partial_transforms.x
    SOURCES ijediMpasPartialTransforms.cc
    LIBS ijedi)
