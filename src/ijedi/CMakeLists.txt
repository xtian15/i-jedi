include_directories(${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND ijedi_src_files
  Traits.h
)

# ###############################################################################
# Project source files
# ###############################################################################
list(APPEND ijedi_src_files

  # Applications
  Applications/GeometryCache.cc
  Applications/GeometryCache.h

  # Geometry
  Geometry/Geometry.cc
  Geometry/Geometry.h
  Geometry/GeometryIterator.cc
  Geometry/GeometryIterator.h
  Geometry/base/GeometryBase.cc
  Geometry/base/GeometryBase.h
  Geometry/base/AtlasMeshBuilderContract.h
  Geometry/atlas/GeometryAtlas.cc
  Geometry/atlas/GeometryAtlas.h
  Geometry/gsibec/GeometryGsibec.cc
  Geometry/gsibec/GeometryGsibec.h
  Geometry/gsibec/GsiGrid.cc
  Geometry/gsibec/GsiGrid.h
  Geometry/mpas/GeometryMPAS.cc
  Geometry/mpas/GeometryMPAS.h
  Geometry/mpas/MpasAtlasGeometry.cc
  Geometry/mpas/MpasAtlasGeometry.h
  Geometry/mom6/GeometryMOM6.cc
  Geometry/mom6/GeometryMOM6.h
  Geometry/mom6/GeometryMOM6Utils.cc
  Geometry/mom6/GeometryMOM6Utils.h

  # Fields
  FieldMetadata/FieldsMetadata.cc
  FieldMetadata/FieldsMetadata.h
  FieldMetadata/FieldsMetadataDefault.h

  # State
  State/State.cc
  State/State.h
  State/StateBackend.cc
  State/StateBackend.h
  State/MpasFieldSetOwner.h

  # One in-process MPAS-PyTorch bridge
  Python/PythonRuntime.cc
  Python/PythonRuntime.h
  Python/MpasPythonAdapter.cc
  Python/MpasPythonAdapter.h
  Python/MpasBackendContext.cc
  Python/MpasBackendContext.h

  # Increment
  Increment/Increment.cc
  Increment/Increment.h

  # Native Atlas runtime operators
  Interpolation/AtlasOperatorReceipt.h
  Interpolation/AtlasMeshIntegrity.cc
  Interpolation/AtlasMeshIntegrity.h
  Interpolation/AtlasCellMeanEndpoint.cc
  Interpolation/AtlasCellMeanEndpoint.h
  Interpolation/LocalInterpolator.cc
  Interpolation/LocalInterpolator.h
  Interpolation/MpasAtlasPointOperator.cc
  Interpolation/MpasAtlasPointOperator.h
  Interpolation/MpasAtlasConservativeOperator.cc
  Interpolation/MpasAtlasConservativeOperator.h

  # Model interface support
  Covariance/ErrorCovariance.h
  ModelAux/ModelAuxControl.h
  ModelAux/ModelAuxCovariance.h
  ModelAux/ModelAuxIncrement.h
  ModelData/ModelData.cc
  ModelData/ModelData.h

  # IO
  Io/IoBase.cc
  Io/IoBase.h
  Io/gmsh/IoGmsh.cc
  Io/gmsh/IoGmsh.h
  Io/mom6/FillMaskedCells.cc
  Io/mom6/FillMaskedCells.h
  Io/mom6/IoMOM6.cc
  Io/mom6/IoMOM6.h
  Io/mom6/ReadMOM6Netcdf.cc
  Io/mom6/ReadMOM6Netcdf.h
  Io/mom6/WriteMOM6Netcdf.cc
  Io/mom6/WriteMOM6Netcdf.h
  Io/unstructured/IoUnstructured.cc
  Io/unstructured/IoUnstructured.h

  # Variable change
  VariableChange/SeaSurfaceTemperature.h
  VariableChange/SeaSurfaceTemperature_A.cc
  VariableChange/VaderCookbook.cc
  VariableChange/VaderCookbook.h
  VariableChange/VariableChange.cc
  VariableChange/VariableChange.h

  # Linear variable change
  LinearVariableChange/LinearVariableChange.cc
  LinearVariableChange/LinearVariableChange.h

  # Model and LinearModel (persistence and identity)
  Model/Model.h
  LinearModel/LinearModel.h

  # Utilities
  Utilities/Constants.cc
  Utilities/Constants.h
  Utilities/ijedi_constants_mod.f90
  Utilities/ijedi_kinds_mod.f90
  Utilities/ijedi_netcdf_utils_mod.F90
  Utilities/PrintHelper.cc
  Utilities/PrintHelper.h
)

# ###############################################################################
# FV3 specific project source files and setup
# ###############################################################################

# fv3 pieces if FMS is found
if(FMS_FOUND)
  list(APPEND ijedi_fv3_cpp_src_files
    Geometry/fv3/GeometryFV3.cc
    Geometry/fv3/GeometryFV3.h
    Geometry/fv3/GeometryFV3Parameters.h
    Geometry/fv3/GeometryFV3.interface.h
    Io/fv3-history/IoFV3History.cc
    Io/fv3-history/IoFV3History.h
    Io/fv3-restart/IoFV3Restart.cc
    Io/fv3-restart/IoFV3Restart.h
    Io/fv3-restart/IoFV3Restart.interface.h
  )
  list(APPEND ijedi_fv3_fortran_src_files
    Geometry/fv3/ijedi_fv3_akbk_mod.f90
    Geometry/fv3/ijedi_fv3_arrays.F90
    Geometry/fv3/ijedi_fv3_control.F90
    Geometry/fv3/ijedi_fv3_eta.F90
    Geometry/fv3/ijedi_fv3_geom_interface_mod.f90
    Geometry/fv3/ijedi_fv3_geom_mod.f90
    Geometry/fv3/ijedi_fv3_grid_tools.F90
    Geometry/fv3/ijedi_fv3_grid_utils.F90
    Geometry/fv3/ijedi_fv3_mp_mod.F90
    Geometry/fv3/ijedi_fv3_namelist_mod.f90
    Geometry/fv3/ijedi_fv3_sorted_index.F90
    Io/fv3-restart/IoFV3Restart.interface.F90
    Io/fv3-restart/ijedi_fv3_restart_mod.f90
  )

  list(APPEND ijedi_fv3_src_files
    ${ijedi_fv3_cpp_src_files}
    ${ijedi_fv3_fortran_src_files}
  )

  # fv3 ported files have specific required properties
  list(APPEND fv3_defs SPMD)
  list(APPEND fv3_defs MOIST_CAPPA)
  list(APPEND fv3_defs USE_COND)
  list(APPEND fv3_defs OVERLOAD_R4)
  list(APPEND fv3_defs OVERLOAD_R8)
  list(APPEND fv3_defs INTERNAL_FILE_NML)
  list(APPEND fv3_defs ENABLE_QUAD_PRECISION)
  set_source_files_properties(${ijedi_fv3_src_files} PROPERTIES COMPILE_DEFINITIONS "${fv3_defs}")

  # Combine with main source files
  list(APPEND ijedi_src_files ${ijedi_fv3_src_files})

  # Compile with double precision
  if(CMAKE_Fortran_COMPILER_ID STREQUAL "Intel")
    set(fv3_flags "-r8")
  elseif(CMAKE_Fortran_COMPILER_ID STREQUAL "GNU")
    set(fv3_flags "-fdefault-real-8 -fdefault-double-8 -ffree-line-length-none")
  else()
    message(WARNING
      "Unknown Fortran compiler '${CMAKE_Fortran_COMPILER_ID}',\n"
      "  fv3 files not using special flags.")
    set(fv3_flags "")
  endif()

  set_source_files_properties(${ijedi_fv3_fortran_src_files} PROPERTIES COMPILE_FLAGS "${fv3_flags}")
endif()

# ###############################################################################
# Model specific project source files
# ###############################################################################
ecbuild_add_library(TARGET ${PROJECT_NAME}
  SOURCES ${ijedi_src_files}
  INSTALL_HEADERS LISTED
  LINKER_LANGUAGE CXX
)

target_include_directories(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>)

# Direct include directories of the selected scientific targets must precede
# every transitive SDK prefix, which may also contain unrelated JEDI headers.
# This applies equally to build-tree and installed-package exports.
# Some build-tree exports include a shared SDK prefix in their own direct
# include list. Promote only namespace-specific roots, leaving shared prefixes
# in the ordinary transitive list. ecbuild disables system treatment of imported
# includes, so marking a shared prefix SYSTEM does not establish this ordering.
set(_scientific_include_roots)
foreach(_dependency oops vader atlas fckit eckit)
  if(TARGET ${_dependency})
    get_target_property(_includes ${_dependency} INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_include IN LISTS _includes)
      if(NOT (EXISTS "${_include}/atlas/library/Library.h" AND
              EXISTS "${_include}/netcdf.h"))
        list(APPEND _scientific_include_roots "${_include}")
      endif()
    endforeach()
  endif()
endforeach()
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC ${_scientific_include_roots})

# target_link_libraries( ${PROJECT_NAME} PUBLIC MPI::MPI_Fortran MPI::MPI_CXX )
target_link_libraries(${PROJECT_NAME} PUBLIC eckit)
target_link_libraries(${PROJECT_NAME} PUBLIC fckit)
target_link_libraries(${PROJECT_NAME} PUBLIC atlas atlas_f)
target_link_libraries(${PROJECT_NAME} PUBLIC oops)
target_link_libraries(${PROJECT_NAME} PUBLIC vader)
# The SDK's shared include prefix also contains older JEDI headers. Put the
# selected source-built graph ahead of that prefix; otherwise NetCDF's include
# directory can silently substitute an unrelated installed Atlas/OOPS API.
target_link_libraries(${PROJECT_NAME} PUBLIC NetCDF::NetCDF_C NetCDF::NetCDF_CXX)
target_link_libraries(${PROJECT_NAME} PUBLIC pybind11::embed)
target_link_libraries(${PROJECT_NAME} PUBLIC OpenSSL::Crypto)
target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json::nlohmann_json)

# target_link_libraries( ${PROJECT_NAME} PUBLIC ufo )
if(FMS_FOUND)
  target_link_libraries(${PROJECT_NAME} PUBLIC fms)
endif()

if(OpenMP_CXX_FOUND)
  target_link_libraries(${PROJECT_NAME} PUBLIC OpenMP::OpenMP_CXX)
endif()
