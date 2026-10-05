# MPAS interface contract

## Geometry and State

MPAS-PyTorch owns topology decoding, native state and nonlinear stepping. I-JEDI
consumes its canonical zero-based snapshot and owns OOPS orchestration. Atlas
compiles the interpolation matrices; forward, transpose and declared-measure
adjoints use the same authenticated coefficients.

Native cell/edge/vertex function spaces use the primal pentagon/hexagon and dual
triangular meshes. Point interpolation uses the dual mesh; conservative remapping
uses the primal mesh. Paired winds transport their basis, including at poles.
Physical heights and pressures are typed fields. The geometry vertical coordinate
is a labelled level index.

Geometry identity binds topology, measures, coordinates, ordered targets, matrix
coefficients and Atlas source identity. Mutation guards bind the actual cached
consumer handles and enforce coherence with the collection lookup map. Traversal
authenticates the whole mesh at its boundary and each returned coordinate without
rescanning all coordinates per point.

State owns one model backend. Atlas fields are generation-bound views. Generic
MPAS `State::read()` and `State::write()` reject before accessing views; restart
uses authenticated serialization. Nonlinear steps check the model inventory,
timestep and support before changing the native state or valid time.

## Dependency boundary

The series pins OOPS 1.13.0 and the declared Atlas 0.46.0 contribution. OOPS needs
its exported-header installation fix; Atlas needs native MPAS topology,
declared-measure operator and FieldSet observer-lifetime support. Those exact
source revisions must be accessible before a remote build is reproducible.
The legacy Dirac implementation uses this pinned OOPS version's GeometryData API;
upstream's newer ProximitySearch API is outside this dependency graph.

## Supported scope

Serial-global pentagon/hexagon MPAS on CPU float64. Unsupported topology,
distributed/regional geometry, GPU and native MPI requests reject before
construction. Spatial TL/AD does not advance a model trajectory. Model-time
TL/AD, covariance, minimization and MPAS data assimilation remain outside this
series.
