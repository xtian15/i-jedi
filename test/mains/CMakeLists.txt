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
