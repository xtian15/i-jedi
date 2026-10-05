# Build and validate the MPAS interfaces

## Prerequisites and inputs

Use native Linux aarch64, GNU 13, CMake with the Unix Makefiles generator, the
pinned JEDI/Atlas dependencies and Python runtime from the checked-in manifests.
C++ arithmetic requires `-ffp-contract=off`; fast-math and reassociation reject.
Keep the SDK read-only. Set `PYTHONDONTWRITEBYTECODE=1`, unset `BASH_ENV`,
`PYTHONPATH` and `LD_PRELOAD` after SDK activation, and set OMP/OpenBLAS/MKL/NumExpr
threads to one with `OMP_DYNAMIC=FALSE`. Serialize scientific runs across the
same physical host using an absolute, shared lock inode. Scaling runs exclude
compilers and other scientific jobs.

Copy `docs/MPAS_INPUTS.cmake.example` to an external `inputs.cmake` and
set its paths to the acquired input bundle. PR1 omits the two PR2 entries. Input files, model wheels,
SDKs and generated reference results are acquired separately and never committed
with the contribution. Their exact receipts and checksums must be accessible to
both the reviewer and CI.

| Variables | Content |
| --- | --- |
| `Python3_EXECUTABLE`, `IJEDI_MPAS_P0_PYTHON` | The same locked Python interpreter |
| `IJEDI_MPAS_P0_WHEEL`, `IJEDI_MPAS_RUNTIME_RECEIPT` | Exact model wheel and runtime identity |
| `IJEDI_MPAS_P0_CASE_DIR` | Complete model input case |
| `IJEDI_MPAS_P0_DIRECT_ORACLE` | Independent direct-model two-step oracle |
| `IJEDI_MPAS_GEOMETRY_CASES` | Ordered three-case native geometry manifest |
| `IJEDI_MPAS_GEOMETRY_LOCATIONS` | Fixed 256- and 10000-target corpora |
| `IJEDI_MPAS_P2_LOCATIONS`, `IJEDI_MPAS_CONTRACT_SOURCE_DIR` | Additional analysis inputs, required by PR2 |
| `IJEDI_POCKETFFT_SOURCE_DIR` | Header/license bytes from the dependency artifact manifest, for source-built Atlas |

Generate the direct oracle with `tools/mpas_bridge/direct_two_step.py`; its
`--help` lists the required input, wheel, timestep and identity arguments. Restore
the Python runtime using `tools/mpas_bridge/recreate_p0_runtime.sh` with the
checksummed wheelhouse. A missing or changed declared input fails configuration.
Atlas/OOPS source pins in `cmake/MpasDependencyPins.cmake` must resolve to the
required contributions; substituting another revision cannot qualify the build.

## Fresh complete gate

Commit the exact candidate before qualification. Configure a fresh build as
shown in the README, then let this wrapper perform the first owned build.
Use `pr1` for geometry/State and `pr2` for the stacked analysis change.

```sh
python3 -B test/tools/test_release_gate.py
python3 -B tools/qualification/run_p0_p2_release_gate.py \
  --source "$PWD" --build "$PWD/build" \
  --manifest "$PWD/docs/P0_P2_REQUIRED_TESTS.json" --stage pr2 \
  --output "$PWD/../qualification" --lock-file "$SHARED_LOCK_FILE" \
  --build-jobs 2 --test-jobs 2
```

The wrapper runs every configured test, including retained non-MPAS cases.
Missing tests, disabled tests, skips, failures, stale owned products, a dirty
checkout, foreign source bindings and changed runtime artifacts fail the gate.
The required-name manifest is a minimum inventory, never a test selection.

The external `../qualification/` directory contains the configured inventory, full JUnit results, test log,
owned executable/library hashes and source-bound receipt. Reauthenticate those
products after any later build or before handing them to another consumer:

```sh
python3 -B tools/qualification/run_p0_p2_release_gate.py \
  --source "$PWD" --build "$PWD/build" \
  --verify-receipt "$PWD/../qualification/qualified.json"
```

A complete I-JEDI pass does not qualify its dependencies or another platform.
Build every dependency target and run its whole configured suite separately;
`tools/qualification/check_dependency_results.py` rejects missing or skipped
results. Final qualification uses two independently rebuilt dependency
installations and fresh builds of both exact PR tips. CI runs the same gate with
a digest-pinned native environment, retains its evidence, and verifies owned
artifacts after the dependency build.

## Scientific checks

The harness retains independent direct-model replay, canonical topology and
analytic-field oracles; exact two-step native-state comparison; cache restoration
and compound mutation attacks; traversal and interpolation cost controls; and
existing FV3/MOM6/Atlas/GSI tests. The required API test includes the
complete public GeometryIterator: five pinned repeats must stay below 256 times
a single full-mesh authentication. A forced scan per point must reject by that
cost bound while preserving the coordinate receipt. This test runs alone. Geometry adjoint and conservative integral
checks enforce the bounds in their native clients, including `2e-14` for the
retained geometry identities.

PR2 also exercises typed requests, view identity, labels and failure atomicity;
nonlinear partial transforms; scalar/vector and control/native adjoints under
declared inner products; and finite-difference curves across states and seeds.
Finite differences must resolve convergence and roundoff, with explicit tests for
null and exactly affine responses. Wrong-transpose and mutated-label controls
must fail. These checks validate spatial transforms on the declared inputs;
model-time derivatives are outside their scope.
