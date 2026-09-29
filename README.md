# Cooling Topology

Generation-bound structural model of data-centre cooling connectivity for the
Data Center Control Plane (DCCP). Cooling Topology is repository 26 of the DCCP
program and is part of Tranche 3, Cooling Infrastructure Control.

The library answers *structural* questions and nothing else. It is deliberately
unable to say that anything is running, flowing, available, eligible or safe.

## The core question

> What cooling infrastructure exists in this exact topology generation, how are
> plants, loops, pumps, chillers, CDUs, CRAH/CRAC units, manifolds, zones and
> dependencies structurally connected, which redundancy relationships are
> possible, and when must a topology claim be rejected as invalid or stale?

Every answer is bound to one immutable, digest-identified topology generation.
A query never consults a mutable store, a clock, a random number or the
environment.

## What this repository owns

* canonical identities, kinds and typed connection points for cooling plants,
  chillers, pumps, hydronic loops, CDUs, CRAH/CRAC units, manifolds, branches,
  thermal zones, cooling sinks and cooling sources;
* directed, typed connectivity among those elements (`supplies`, `returns`,
  `serves`, `pumps`, `contains`, `depends_on`) with an explicit endpoint
  compatibility matrix;
* containment and serving relationships and declared structural dependencies;
* declared redundancy groups and declared structural changeover arrangements,
  including structural independence checks;
* topology generations, immutable publication, verification, diffing and
  conservative recovery with rollback protection and stale-generation fencing;
* deterministic reachability, dependency, path, connectivity and redundancy
  queries over a generation;
* validation of graph invariants with a stable, documented precedence order.

## What this repository explicitly does not own or claim

None of the following is modelled, computed or implied anywhere in the public
API. Each is owned by an adjacent component or by the facility control system,
and arrives here only as an opaque reference or as a generation-stamped
`EvidenceBinding` that is recorded verbatim and never interpreted:

| Excluded claim | Owner |
| --- | --- |
| whether a plant, pump, chiller or fan is running | device and plant control (facility BMS/DCIM, Cooling Failover) |
| whether coolant or air is currently flowing | coolant and airflow instrumentation |
| whether a path has usable cooling capacity | Cooling Capacity, Cooling Capacity Accounting, Rack Capacity |
| whether a redundant source is operationally eligible | Cooling Failover |
| whether a zone is thermally safe | Thermal Zone Manager, Thermal Governor, Thermal Control Plane |
| authority to actuate | Liquid Cooling Control, Airflow Control, PDU Control, UPS Control |
| authority to fail over | Cooling Failover |
| airflow policy and set points | Airflow Control |
| liquid-cooling control loops and set points | Liquid Cooling Control |
| facility placement | Facility Placement Planner, Physical Location Registry |
| power state | Power Topology, Power Control Plane, PDU Control, UPS Control |

The claim boundary is enforced by types, not by convention:

* every public result type carries an `EvidencePosture`;
* `EvidencePosture::claim_disposition(ExcludedClaim)` is a `static constexpr`
  function that returns `ClaimDisposition::NotOwned` for all eleven excluded
  claims and has no state, so no code path can produce any other value;
* the only classification a structural answer can carry is `ClaimClass`, whose
  two values are `StructurallyPossible` and `StructurallyImpossible`;
* no field, accessor or enum value in the public API names availability, health,
  readiness, flow, capacity, safety or authorization.

## Architecture

```
include/dccp/cooling_topology/
  limits.hpp      documented bounds for every externally influenced size
  result.hpp      ErrorCode, ErrorCategory, Error, Result
  text.hpp        strict UTF-8, escaping, canonical integer parsing
  units.hpp       MilliCelsius: exact integer temperature with checked arithmetic
  strong_id.hpp   StrongId<Tag> identities and the distinct counter types
  digest.hpp      SHA-256 and Digest
  evidence.hpp    the claim boundary and generation-stamped external evidence
  model.hpp       node kinds, ports, edge kinds, containment, redundancy
  topology.hpp    draft, header, validation report, immutable Topology
  canonical.hpp   canonical encoding, generation file framing, ordering
  query.hpp       reachability, dependency, path, redundancy and zone queries
  diff.hpp        deterministic diff of two generations
  mutation.hpp    mutation authority and the mutation content fingerprint
  store.hpp       durable generations, publication, verification, recovery
  import.hpp      the "ctg" text grammar and its exporter
  version.hpp     version, systems boundary, component id
```

The implementation is `src/` (19 translation units) plus `tools/ctopctl`,
`examples/`, `benchmarks/` and `tests/`. Only `src/file_ops.cpp` touches
operating-system interfaces.

### Node kinds

`cooling_plant`, `chiller`, `pump`, `cooling_loop`, `cdu`, `crah`,
`crac`, `manifold`, `branch`, `thermal_zone`, `cooling_sink`,
`cooling_source`. There is no generic "device" node: every element of the graph
is one of these twelve kinds, and the node kind is carried by the attribute
variant itself, so an attribute payload can never disagree with its kind.

### Ports and endpoint compatibility

Every distribution or conversion element carries the same four hydronic ports
and one generic terminal:

| Port | Meaning |
| --- | --- |
| `supply_out` | outbound: cooled medium leaves this element downstream |
| `source_in` | inbound: cooled medium enters this element from upstream |
| `heat_out` | outbound: warmed medium leaves this element toward rejection |
| `return_in` | inbound: warmed medium enters this element from downstream |
| `terminal` | the generic connection point of a dependency edge and the installation point of a pump |

plus `container`, `contained`, `server` and `served` for containment and
service relations. Which ports a kind carries is a normative table in
`model.cpp`; `port_allowed_for_kind` exposes it.

A heat-exchange element has two hydronic sides and uses the same four ports on
both: its cooling side is fed by `source_in` and delivers through
`supply_out`, and its heat-rejection side is fed by `return_in` and rejects
through `heat_out`. Which loop is on which side is determined by the loop's
declared kind and medium, and the medium-continuity rule rejects a mismatched
pairing.

### Edge kinds

| Edge | Direction | First endpoint | Second endpoint |
| --- | --- | --- | --- |
| `supplies` | cooled medium flows downstream | supplier kind at `supply_out` | consumer kind at `source_in` |
| `returns` | warmed medium flows back | heat-rejecting kind at `heat_out` | return-target kind at `return_in` |
| `serves` | thermal service | server kind at `server` | zone or sink at `served` |
| `pumps` | installation | loop, manifold or branch at `supply_out` or `return_in` | pump at `terminal` |
| `contains` | enclosure | container kind at `container` | contained kind at `contained` |
| `depends_on` | structural dependency | any kind at `terminal` | any kind at `terminal` |

Supplier kinds: cooling source, cooling plant, chiller, cooling loop, manifold,
branch, CDU. Consumer kinds: cooling plant, chiller, cooling loop, manifold,
branch, CDU, CRAH, CRAC, cooling sink. Heat-rejecting kinds: cooling plant,
chiller, cooling loop, manifold, branch, CDU, CRAH, CRAC, cooling sink.
Return-target kinds: cooling source, cooling plant, chiller, cooling loop,
manifold, branch, CDU. Server kinds: CRAH, CRAC, CDU, cooling loop, manifold,
branch. Pump hosts: cooling loop, manifold, branch.

Containment pairs are an explicit matrix (plant contains chiller, pump, loop,
manifold, CDU; loop contains pump, manifold, branch, chiller, CDU; chiller
contains pump; CDU contains pump; manifold contains branch, pump, CDU; branch
contains pump, CDU; thermal zone contains sink, CRAH, CRAC, CDU). Any other pair
is rejected.

### Cycles

* `supplies` must be a directed acyclic graph, and `returns` must be a
  directed acyclic graph.
* Their **union may and normally does contain a cycle**: that cycle is the
  physical hydronic circuit, and `possible_circuits` reports it as a supply path
  plus a return path.
* `contains` must be a forest: at most one container per element, no cycles.
* `depends_on` must be acyclic, and `depends_on` together with `contains`
  must be acyclic, so a contained element can never be a structural dependency of
  its own container.
* `serves` must be acyclic. This is additionally impossible by construction: a
  serving kind has a `server` port and no `served` port, and a served kind has
  a `served` port and no `server` port, so the port matrix partitions the graph
  and a service cycle cannot be expressed. The validator keeps a defensive
  `ServiceCycle` check and the test suite proves the matrix that makes it
  unreachable.

Every cycle search is an iterative colour-marked depth-first search; nothing in
the library recurses over graph depth.

### Structural completeness rules

* a `branch` must be contained in exactly one distribution element;
* a `cooling_sink` must be located in exactly one thermal zone;
* a `pump` must be installed on exactly one distribution element through
  exactly one `pumps` edge;
* a statement that something is missing is never invented: the three completeness
  findings below are **warnings**, they never gate publication, and they are
  always reported after every error.

## Redundancy

A redundancy group declares `scheme` (N, N+1, 2N, 2N+1, distributed redundant,
concurrently maintainable), `scope`, its members with their declared spelling
and optional failure-domain reference, and the independence properties the
arrangement claims.

Scope constrains member kinds: `plant` (cooling plant), `chiller`,
`pump`, `cooling_loop`, `distribution` (manifold, branch, CDU),
`source_path` (cooling source), `air_handling` (CRAH, CRAC). A group that
mixes unrelated kinds is rejected.

Two declarations can be checked against the graph:

* `require_distinct_failure_domains` - every member must declare a failure domain
  and no two members may declare the same one;
* `require_independent_sources` - no two members may share a structural source,
  every member must have at least one, and no member may be a structural ancestor
  of another.

A structural ancestor that is not itself an origin necessarily shares a source
with its descendant, so the shared-source rule already detects it; the report
separately names members that are origins feeding another member. A pump's
structural sources are the sources of the distribution element it is installed
on, because a pump is attached through a `pumps` edge rather than a
`supplies` edge.

Redundancy here describes **declared structural independence**. It is never a
statement about operational readiness, eligibility, capacity or failover
authority. `RedundancyVerdict` has exactly three values:
`DeclarationConsistent`, `DeclarationViolated` and `IndependenceUnproven`.

## Authority, generations and fencing

Distinct concepts are distinct types and cannot be mixed:

| Type | Meaning |
| --- | --- |
| `TopologyGeneration` | monotonic counter of published state; 0 means nothing published |
| `DraftRevision` | revision of an in-memory, un-published draft |
| `WriterEpoch` | durable mutation-authority epoch of a store |
| `WriterIncarnation` | identity of one concrete mutation-authority holder |
| `AttemptOrdinal` | 1-based ordinal of one attempt at a mutation |
| `CommitSequence` | persistence counter assigned at the commit point |
| `ExternalGeneration` | generation of an external registry binding |
| `AuthorityEpoch` | epoch of an adjacent supervision plane (provenance only) |
| `EvidenceGeneration` | generation of an external observation supplied as evidence |
| `ObservationSequence` | sequence of an observation or effect owned elsewhere |

A mutation is planned against a `MutationAuthority` (epoch, incarnation,
expected base generation). The store refuses, deterministically and in this
order:

1. a replay of an accepted attempt returns the recorded receipt with
   `replayed = true` and performs no re-actuation - this check runs *before*
   every authority check, so a retry of a lost response is never mistaken for a
   stale write;
2. the same mutation identity with a different content digest is
   `IdempotencyConflict`;
3. a mismatched epoch is `StaleAuthorityEpoch`, a mismatched incarnation is
   `StaleWriterIncarnation`, and an expected base that is not the head is
   `StaleBaseGeneration`; nothing is staged;
4. a draft bound to a different facility is `StoreMismatch`.

The mutation content fingerprint covers the canonical image of the facility
binding, the provenance and every table in canonical order, and deliberately
excludes the authority. A draft that cannot be canonicalized is fingerprinted
from its own rejection, so two different malformed drafts cannot collide into a
replay.

Recovered durable state is never presented as fresh physical evidence. A store
reports `StoreOpenState::Recovered` when it adopted a retained publication, and
recovered evidence is only ever an `EvidenceBinding` naming the generation it
was taken from.

## Canonical form and determinism

Format version 1 of the canonical image is little-endian, fixed width, with
length-prefixed strings, presence bytes for optionals, variant tags for node
attributes and explicit table counts. Every table is written in canonical order
(sorted by identity), member order inside a declaration is normalized, and no
timestamp, address, process id or random value appears anywhere. Equivalent
logical state therefore produces identical bytes in every process, and
encode/decode/encode is a fixed point that `Topology::decode` enforces.

Generation files are framed as
`magic[8] "CLDTOPG1" | schema u16 | reserved u16 (must be 0) | payload_len u64 |
payload | sha256(payload)[32]`, and the decoder rejects a wrong magic, an
unsupported schema version, a non-zero reserved field, a length that disagrees
with the frame, a digest mismatch, truncation and trailing bytes.

Canonical ordering is what makes results deterministic: every query sorts its
answers by identity, every validation issue is sorted within its stage, and no
observable order depends on container iteration.

## Validation precedence

A draft is validated in fourteen ordered stages and the first failing stage
wins, so a request with several defects always reports the same primary error:

| Stage | What it decides |
| --- | --- |
| shape | bounds, identifier syntax, display text, enum payload validity, temperature range, sink consumer kind |
| identity | duplicate identities, alias id collisions, alias chains and missing alias targets |
| endpoint | endpoint existence, self edges, duplicate edges |
| role | kind relation, then per-kind port legality, then the port roles the edge kind requires |
| circuit | `supplies` acyclicity and `returns` acyclicity |
| medium | medium continuity, declared design-temperature compatibility, primary/secondary loop direction |
| containment | single container, containment matrix, mandatory container, containment acyclicity |
| attachment | pump installation cardinality |
| service | serves endpoint legality and acyclicity |
| redundancy | member resolution, scope, double counting, failure domains, independence |
| changeover | changeover cardinality, membership uniqueness, member port role, attachment to a real connection |
| dependency | `depends_on` acyclicity alone and together with containment |
| binding | facility reference, provenance, evidence bindings |
| completeness | warning-only: isolated element, unfed sink, unserved zone |

`ValidationReport` exposes `valid()` (no error-severity issue),
`error_count()`, `warning_count()` and `primary()`. `Topology::create`
returns the primary issue as an `Error` whose code, message and subject are the
machine contract; the remaining error issues are attached as details. Error
message text is never the contract: callers switch on `ErrorCode`.

## Persistence and recovery

```
<root>/
  manifest          authoritative head record (CLDT-MANIFEST 1, checksummed)
  manifest.prev     previous committed head record
  floor             durable monotone generation floor (CLDT-FLOOR)
  lock              exclusive OS-level writer lock (diagnostic text)
  generations/      immutable framed generation files, g<generation>-<digest>.ctgen
  idem/             accepted-attempt records (CLDT-IDEM) for idempotent replay
  staging/          transient staging area, empty between publications
  quarantine/       content that could not be verified, moved aside, never deleted
```

Publication protocol and its **single commit point**:

1. validate the request authority and the idempotency record;
2. validate and build the draft against the expected base generation;
3. reserve the generation number and the commit sequence;
4. frame the canonical image and write it into `staging/`, then flush;
5. read it back, decode it, re-validate it and compare digests;
6. atomically rename the staged file into `generations/`;
7. write the *pending* accepted-attempt record before the commit;
8. **commit point:** atomically replace `manifest` (and move the previous
   manifest to `manifest.prev`);
9. advance the durable `floor`, retire content below it, remove staging residue;
10. mark the accepted-attempt record accepted and bound the record history.

A crash before step 8 leaves the previous complete generation authoritative and
the interrupted attempt resolvable; a crash at or after step 8 leaves the new
complete generation authoritative. Recovery never produces a hybrid: the head is
re-read, integrity-checked, re-validated and required to match its manifest
digest, and the canonical image must be a fixed point of re-encoding.

`recover()` reports `NoAction` when the head verifies and `AdoptedPrevious`
when the retained previous committed publication is adopted instead. A refusal to
recover is an error, not an outcome: when nothing can be adopted the call fails
with `RecoveryUnavailable`, `HeadCorrupt` or `GenerationFloorViolation` and the
store stays unverifiable, so a successful report never describes a store that is
still broken. Recovery never adopts below the durable generation floor and it
moves unverifiable content into `quarantine/` rather than deleting it.

Idempotency is bounded by `StoreOptions::idempotency_retention`. An attempt
whose record has been evicted is no longer recognizable as a replay and is
treated as a new mutation; the store never claims to have detected an eviction.

The store root is canonicalized: it must be absolute, contain no `..`, and be
free of reparse points on every ancestor, so two processes cannot obtain two
different locks for the same logical store.

## Concurrency model

The library is single-threaded internally: a `Topology` is an immutable value
and every query is a pure function of it. The only shared resource is a store
directory, and exclusion is at the OS level:

* one writer at a time, through an exclusive advisory lock held for the lifetime
  of the handle; a second writer fails with `StoreLocked` rather than blocking;
* unlimited read-only handles coexisting with a writer;
* a writer lock released by process death, not only by an orderly close.

No lock is ever taken recursively, no callback is invoked while a lock is held,
no user code runs inside a locked region, and no internal call re-enters a locked
`Store` method. Ownership was audited by inspection of every call path in
`store.cpp` and `file_ops.cpp`, and the process-death and second-writer cases
are proven with real child processes.

The library has no background work, no worker threads, no queues and no
asynchronous completion path. There is therefore no cancellation or shutdown
surface, and the defect classes that belong to one (cancelled work publishing a
late success, joining a worker while holding a resource it needs, lock inversion
during shutdown, unbounded retry state) cannot arise. Repeated
`Store::open`/`Store::close` is safe: the lock, the file handles and the staging
area are released and re-acquired on every cycle.

## Resource bounds

Every externally influenced size is bounded before allocation, and the bounds are
part of the public contract in `limits.hpp`: identity and text lengths, table
sizes (100000 nodes, 200000 edges, 4096 groups, 32768 aliases), canonical image
size (64 MiB), traversal limits (depth 512, 100000 nodes, 16384 results, 512
paths), import limits (32 MiB, 8192 bytes per line, 400000 lines) and persistence
limits (8 retained generations, 64 accepted-attempt records, 64 KiB manifest).
The decoder checks every declared count against both its limit and the bytes that
actually remain, so a malicious count can never drive an allocation.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Options: `COOLING_TOPOLOGY_BUILD_TESTS`, `COOLING_TOPOLOGY_BUILD_TOOLS`,
`COOLING_TOPOLOGY_BUILD_EXAMPLES`, `COOLING_TOPOLOGY_BUILD_BENCHMARKS`,
`COOLING_TOPOLOGY_WARNINGS_AS_ERRORS` (default ON, `/W4 /WX` on MSVC) and
`COOLING_TOPOLOGY_ENABLE_ASAN`.

## Install and use from another project

```sh
cmake --install build --config Release --prefix /some/prefix
```

The install exports the namespaced target `dccp::cooling_topology`, the public
headers, the `ctopctl` tool and the package configuration. A consumer project
uses it with:

```cmake
find_package(cooling_topology 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::cooling_topology)
```

`tests/downstream` is a complete out-of-tree consumer that configures against
the installed prefix, links the exported target and runs a real library lifecycle
without including anything from this repository's source tree.

## Command-line tool

```sh
ctopctl version
ctopctl posture                       # the full claim boundary, one line per claim
ctopctl validate facility.ctg
ctopctl show facility.ctg
ctopctl export facility.ctg
ctopctl sources facility.ctg
ctopctl upstream facility.ctg <node>
ctopctl downstream facility.ctg <node>
ctopctl paths facility.ctg <from> <to>
ctopctl circuits facility.ctg <from> <to>
ctopctl independent facility.ctg <from> <to>
ctopctl spof facility.ctg <node>...
ctopctl components facility.ctg
ctopctl sink facility.ctg <node>
ctopctl zone facility.ctg <node>
ctopctl blast facility.ctg <node>
ctopctl group facility.ctg <group-id>
ctopctl membership facility.ctg <node>
ctopctl diff before.ctg after.ctg
ctopctl store <dir> [verify|history|head|recover]
```

Exit codes: 0 success, 1 structural or validation rejection, 2 usage error, 3 I/O
or store error. Every structural answer is followed by the claim-boundary
statement.

## Text grammar

```
facility <extref>
provenance producer=<text> origin=<token> witness=<text>
evidence <kind> producer=<text> subject=<extref>
node <id> <kind> [key=value ...]
edge <edge-id> supplies <node>.<port> -> <node>.<port>
edge <edge-id> returns  <node>.<port> -> <node>.<port>
edge <edge-id> serves   <node>.<port> -> <node>.<port>
edge <edge-id> pumps    <node>.<port> -> <node>.<port>
edge <edge-id> contains <node>.<port> -> <node>.<port>
edge <edge-id> depends  <node>.<port> -> <node>.<port>
alias <alias-id> <node-id>
group <group-id> scope=<token> [scheme=<token>] [flags] <member>[@<failure-domain>] ...
changeover <group-id> max=<n> <node>.<port> ...
```

`#` starts a comment outside quoted text; a token that contains spaces, `#`,
`=`, `:` or `@` is quoted with the escape syntax of `escape_text()`, and
`export_import` quotes exactly those tokens, so an exported document always
re-parses to the same canonical digest. Parsing never validates structure: the
draft is validated by `Topology::create`, which reports the primary structural
error.

## Examples and benchmark

```sh
./build/examples/cooling_topology_example_plant_redundancy
./build/examples/cooling_topology_example_primary_secondary_circuit
./build/examples/cooling_topology_example_stale_update
./build/examples/cooling_topology_example_durable_store
./build/benchmarks/cooling_topology_benchmarks [scale]
```

The benchmark measures completed useful operations (generation build, canonical
encode, canonical decode, draft validation, supply-path query, independent-path
query, connected components, and a durable store publication *including* its
durability cost). It states the workload sizes, the iteration count and whether
each workload is REAL (actual process and filesystem work on this host) or
SYNTHETIC (a simulated facility topology).

## Real versus synthetic proof

* **REAL** - the software behaviour actually exercised on the build host: the
  library, the canonical codec, the validation engine, the durable store, its
  crash and recovery behaviour, the exclusive lock across real OS processes, the
  install and the out-of-tree package consumption. All of these were run here.
* **SYNTHETIC** - every facility topology used in tests, examples and the
  benchmark is a simulated graph constructed by the test fixtures. It is a
  realistic structural shape, not a model of any real facility.
* **UNSUPPORTED** - no data-centre cooling plant, chiller, pump, CDU, CRAH/CRAC
  unit, manifold, BMS/DCIM interface, PLC or physical plant controller was
  available. Nothing here is hardware validation, and no claim about a physical
  facility is made or implied.

## Validation performed

The test suite is a single executable that runs every proof obligation to
completion and reports its own findings. All of the following was observed on
the development host: Windows x64, MSVC 19.44
(Visual Studio 2022 Build Tools 17.14) and CMake 4.3.2, with the generator
"Visual Studio 17 2022".

| Configuration | Build | Suite result |
| --- | --- | --- |
| Release | zero compiler diagnostics under `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc` | 206 tests, 0 failing checks (10 consecutive runs) |
| Debug | zero compiler diagnostics | 206 tests, 0 failing checks |
| Release, AddressSanitizer | zero compiler diagnostics | 206 tests, 0 failing checks, no sanitizer report |
| `ctest --output-on-failure` | - | 1/1 test passed |

The same suite passes with `--seed=1`, `--seed=987654321` and
`--seed=123456789012345`; randomized cases derive their seed from the run seed
and the case name, so any failure is reproducible from the seed it prints.

AddressSanitizer is genuinely active in the third configuration: with the ASan
runtime removed from the process search path the test binary fails to start
(0xC0000135), which is only possible if the instrumented objects really do link
the runtime. MSVC's AddressSanitizer does not implement leak detection, so no
leak-check result is claimed.

Crash consistency was exercised with real child processes: a child was
terminated at each of the twelve documented fault-injection points, under both
naming schemes, and after every kill a freshly started process resolved the
store to either the previous complete generation or the new complete generation,
decoded and re-validated the head, matched it against the manifest digest, and
verified the parent chain and the commit sequence. Exclusivity and staleness were
exercised across real processes as well: a second writer is refused with
`StoreLocked`, the lock is released by process death and not only by an orderly
close, a superseded epoch and incarnation are fenced, and a read-only reader
running against a publishing writer never observed a partial generation.

Packaging was validated end to end: `cmake --install` into a prefix, then an
independent out-of-tree project configured with
`find_package(cooling_topology 1.0 REQUIRED)`, linked against the exported
`dccp::cooling_topology` target, building and running a complete lifecycle
(parse, build, frame, decode, query, durable publish, head read-back, verify)
using only the installed headers and library. The `ctopctl` tool was exercised
over every documented command: sixteen commands exit 0, five malformed
invocations exit 2, and an absent store exits 3. All four examples and the
benchmark exit 0.

The benchmark measured the following completed operations on this host for the
stated synthetic facility (124 nodes, 484 edges, 44532 document bytes, 27658
canonical bytes, 27710 generation-file bytes):

| Case | Workload | Iterations | us per completed operation |
| --- | --- | --- | --- |
| build_generation | `Topology::create` of the draft | 16 | 566.8 |
| canonical_encode | `canonical_bytes()` | 100 | 26.8 |
| canonical_decode | `Topology::decode()` of the framed file | 16 | 916.7 |
| validate_draft | `validate_draft()` | 32 | 329.7 |
| supply_paths | `possible_supply_paths` source to sink | 200 | 35.4 |
| independent_paths | `independent_supply_paths` source to sink | 100 | 34.7 |
| components | `connected_components()` | 200 | 80.8 |
| publish_durable | `Store::publish` with `durable_flush` | 4 | 21189.5 |

The durable publication figure is a REAL workload: it includes the staging
write, the flush, the read-back verification, the atomic replacement and the
manifest commit. The other figures are SYNTHETIC workloads over a simulated
facility. These are single-host measurements of this implementation; no
before/after claim is made and no number here is a promise about another machine.

What could not be obtained in this environment is stated plainly: there is no
data-centre cooling plant, chiller, pump, CDU, CRAH/CRAC unit, manifold, BMS or
DCIM interface, PLC or plant controller available, so no hardware behaviour,
no live telemetry and no real facility topology was exercised. Nothing in this
repository claims otherwise.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
