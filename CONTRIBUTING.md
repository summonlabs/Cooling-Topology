# Contributing to Cooling Topology

Cooling Topology is part of Data Center Control Plane (DCCP), Tranche 3 (Cooling
Infrastructure Control), and is maintained by Summon Software Labs.

## Licensing of contributions

This project is licensed under the Apache License, Version 2.0 (see `LICENSE`).

By submitting a contribution you agree that it is licensed under the terms of
that license, as described in section 5 of the license text. There is **no
Contributor License Agreement** to sign, and no copyright assignment is required:
you keep the copyright in your contribution and grant the project the license
described in `LICENSE`.

Please do not add co-author trailers or attribution lines that name tools,
assistants or intermediate processes; commit authorship is the responsibility of
the human contributor.

## What belongs in this repository

Cooling Topology owns the generation-bound structural graph of data-centre
cooling connectivity: cooling plants, chillers, pumps, hydronic loops, CDUs,
CRAH/CRAC units, manifolds, branches, thermal zones, cooling sinks and cooling
sources; the directed and typed connections among them; containment and
dependency relations; declared redundancy groups and changeover arrangements;
immutable topology generations; deterministic canonical serialization; durable
generations with an authoritative head manifest; verification, diffing and
conservative recovery.

It deliberately does **not** own, and must not grow:

* whether a component is currently running (device and plant control);
* whether coolant or air is currently flowing (facility instrumentation);
* whether a path has usable cooling capacity (Cooling Capacity, Cooling Capacity
  Accounting, Rack Capacity, Facility Capacity);
* whether a redundant source is operationally eligible (Cooling Failover);
* whether a zone is thermally safe (Thermal Zone Manager, Thermal Governor,
  Thermal Control Plane);
* actuation authority (Liquid Cooling Control, Airflow Control, PDU Control, UPS
  Control);
* failover authority (Cooling Failover);
* airflow policy, containment pressure or set points (Airflow Control);
* liquid-cooling control loops and set points (Liquid Cooling Control);
* facility placement (Facility Placement Planner, Physical Location Registry);
* power state (Power Topology, Power Control Plane, PDU Control, UPS Control).

External facility, rack, asset, location, consumer and failure-domain identities
may be referenced only through the opaque `ExternalRef` type. Those bytes are
preserved exactly and are never resolved, normalized or interpreted here.

Dynamic telemetry and mutable device operating state must not be added to the
canonical topology. A generation is a structural claim about what exists and how
it is connected, not an observation of what is running, flowing or safe. Evidence
owned by another component enters this library only as an `EvidenceBinding`
record, which is stored verbatim and never interpreted.

## Building and testing

The project is a portable C++20 CMake project with no third-party dependencies:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Requirements: CMake 3.20 or newer and a C++20 compiler (MSVC 19.30+, GCC 11+ or
Clang 14+).

Useful options:

* `-DCOOLING_TOPOLOGY_WARNINGS_AS_ERRORS=ON` (default) - first-party warnings are
  errors; do not add warning suppressions, fix the cause;
* `-DCOOLING_TOPOLOGY_ENABLE_ASAN=ON` - AddressSanitizer build;
* `-DCOOLING_TOPOLOGY_BUILD_TESTS/TOOLS/EXAMPLES/BENCHMARKS=OFF` - skip optional
  components when embedding the library.

The test suite is a single executable that runs every proof obligation to
completion and reports its own findings:

```sh
./build/cooling_topology_tests
./build/cooling_topology_tests --filter=store --seed=12345
```

Property and randomized cases derive their seed from the run seed and the case
name; a failure prints the reproduction seed, and `--seed=<n>` reproduces it
exactly. Never weaken a case to make a build pass: a failing case is a defect to
diagnose and repair.

## Code quality expectations

* C++20, standard library only. A new third-party dependency must be justified in
  the pull request, packaged and validated; the default answer is no.
* The public headers are a long-lived contract: add, do not renumber. Error codes,
  tokens and file-format fields are stable once released.
* Every state-dependent mutation states the authority and base generation it was
  planned against and must refuse stale authority rather than merge it.
* Deterministic outcomes only: no wall-clock values, no randomness, no
  process-specific values and no unordered iteration order in canonical content.
* Bound every externally influenced size before allocation and use checked
  arithmetic for authoritative calculations.
* Keep the library single-threaded internally; do not add concurrency without a
  documented lock order and a proof of safety.
* Keep the claim boundary explicit in the API and in the documentation: this
  project answers structural questions and never claims operating state, flow,
  capacity, eligibility, thermal safety or actuation.

## Before opening a pull request

1. `cmake --build build` is warning-free with warnings-as-errors enabled.
2. The full test suite passes, including the new obligations your change adds.
3. Examples and the `ctopctl` CLI still build and run.
4. `cmake --install` plus an out-of-tree `find_package(cooling_topology)`
   consumer still works when you changed anything public.
5. Public documentation (`README.md`) describes only implemented and verified
   behaviour.
