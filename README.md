# Interface to JEDI

I-JEDI supplies model interfaces for OOPS. This series adds MPAS-PyTorch on
serial-global CPU float64 while retaining the FV3, MOM6, Atlas and GSI interfaces.

The geometry change builds native MPAS primal/dual Atlas meshes, authenticates
mutable geometry handles, compiles point and conservative interpolation, and
couples model-owned State storage to nonlinear OOPS steps and authenticated restart.
The stacked analysis change adds typed Increments, native analysis writes and
spatial nonlinear, tangent-linear and adjoint variable transforms.

Read [the interface contract](docs/MPAS_INTERFACE.md) for ownership and supported
operations, and [validation](docs/VALIDATION.md) for inputs, commands and acceptance.

## Build

Use an out-of-source CMake build with the pinned dependencies in
`bundle/CMakeLists.txt` and `cmake/MpasDependencyPins.cmake`. The MPAS runtime is
Python 3.11 with the exact hash-locked CPU packages and model wheel described in
`tools/mpas_bridge/p0_runtime_receipt.json`. A complete MPAS build also needs the
authenticated input cache described in the validation guide.

```sh
cmake -G "Unix Makefiles" -S . -B build -C inputs.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-ffp-contract=off \
  -DCMAKE_PREFIX_PATH="$DEPENDENCY_PREFIX;$SDK_PREFIX" \
  -DIJEDI_MPAS_RELEASE_STAGE=pr2 -DBUILD_TESTING=ON
```

Run the release wrapper from the validation guide to build and test fresh owned
products. For legacy interfaces without MPAS qualification, omit the release
stage, then use `cmake --build build` and `ctest --test-dir build`.

MPAS distributed/regional/GPU execution and model-time TL/AD are unsupported.
These interfaces do not establish assimilation or forecast skill.

## License

See [LICENSE](LICENSE), [DISCLAIMER](DISCLAIMER) and
[third-party notices](THIRD_PARTY_NOTICES.md).
