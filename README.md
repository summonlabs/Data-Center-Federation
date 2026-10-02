# Data Center Federation

Data Center Federation is the boundary of the Summon Software Labs Open Source
Data Center Model that composes independently governed data-center sites into a
larger authority domain. It owns federation membership, federation generations,
site delegation, compatibility admission, partition-safe federation state, and
reconciliation after disconnection.

It is one boundary in the Data Center Control Plane (DCCP). It is built to be
useful on its own and swappable: nothing here reaches into another runtime, and
nothing here assumes that the site on the other end is the same implementation.

## What this boundary owns, and what it does not

Owned here:

- the federation identity and its generation counter;
- the membership record of every site, and the lifecycle that record moves through;
- compatibility admission: versioned, capability-based declarations with staged
  compatibility windows;
- delegation grants: scope, generation, expiry, revocation, and the survivability
  rule that decides what a grant means across a partition;
- the federation's view of connectivity and partition;
- reconciliation records produced when a disconnected site reconnects;
- durable idempotency receipts, so a retried request cannot be applied twice.

Explicitly not owned here:

- local site operations. A site keeps running while the federation cannot reach
  it, and this runtime never reaches into a site;
- placement, capacity brokerage, and reservations;
- disaster-recovery sequencing;
- global policy semantics.

References to an adjacent boundary - a policy domain, a capability name, a
resource selector - are typed references. They are stored, compared, ordered and
hashed. They are never dereferenced, and the presence of an integration is never
treated as permission to read another runtime's state.

## Architecture

```
include/dcf/          public headers, installed with the package
src/                  the runtime
  types, codec, hash  strong identifiers, canonical byte encoding, SHA-256, CRC-32C
  capability          compatibility declarations, windows, explainable refusal
  delegation          scopes, grants, revocation, exclusive-conflict detection
  membership          the lifecycle table and the membership record
  reconciliation      what a reconnecting site means
  command, state      the command vocabulary and the authoritative state
  engine              the deterministic state machine: evaluate, then commit
  store               the crash-safe journal and snapshot
  runtime             the single-mutator threaded service
  wire                the framed protocol and the TCP transport
  report, json, cli   operator output and command construction
apps/                 dcf-federationd, dcf-sited, dcfctl
tools/dcf-relay       a byte relay that makes partitions real
examples/consumer     an independent downstream consumer
bench/                the benchmark harness
tests/                the suites, including the multiprocess one
```

The engine is a pure, single-threaded state machine with no threads, no locks and
no I/O. The runtime owns durability and concurrency. Keeping those out of the
engine is what makes recovery and the live path the same code, and what makes the
engine exhaustively testable.

## Data model and invariants

Identifiers are opaque 128-bit values in distinct C++ types, so a federation
identifier cannot be passed where a site identifier is expected. Generations are
monotonic counters that report exhaustion instead of wrapping.

The canonical byte encoding is shared by the durable journal, the wire protocol
and the state digest. There is exactly one encoding of a value, so a digest is a
function of the value and not of the path that produced it.

The state exposes two digests over the same data:

- the **authority digest** covers exactly the fields that can change the
  federation generation;
- the **canonical digest** covers everything, including observations and
  receipts, and is used to verify a snapshot.

Because the authority digest covers exactly the generation-bearing fields, two
states that share a generation and an authority digest are the same authority.
That is the property reconciliation relies on.

Every collection is ordered by a total, canonical key order, so iteration,
serialization and hashing are the same operation.

## Authority and generations

- The **federation generation** advances by exactly one for each committed
  journal entry that changes authority. Recording what a site reported, and
  recording a reconciliation outcome, are evidence rather than authority: they do
  not advance it. If they did, a site that had just synchronised would be behind
  again the moment its own report was written.
- The **membership generation** belongs to one site and advances on each accepted
  membership transition for that site. A delegation is stamped with the
  membership generation it was issued against, so a grant cannot be resurrected
  for a later incarnation of a site.
- The **journal sequence** advances by exactly one per durable record of any
  kind, and is the durable ordering.
- **Revocation is monotonic.** A revoked grant is never restored by a later
  message, and re-revoking keeps the earliest revocation, so a replay cannot move
  the boundary later.
- **Removal is terminal.** A removed member that sends a rejoin is fenced: the
  federation records the reconciliation, tells the site to adopt the current
  generation, and tells it that it remains outside. It is never quietly
  reinstated.
- A site that claims a generation the federation has never issued is reported as
  @federation_regression@. The federation does not adopt the claim and does not
  demote the site; it reports @indeterminate@ and refuses to reconcile
  automatically, because a site that is ahead is describing authority this
  federation may have lost.

## Membership lifecycle

```
candidate -> compatibility_validated -> admitted -> active
                                            |          |
                                            v          v
                                        draining <- constrained
                                            |          |
                                            v          v
                                         removed <- partitioned

refused_incompatible, refused_conflict, refused_authority are terminal.
```

The transition table is total: any pair not listed is illegal, including every
transition out of a terminal state. Every command that would move a site is
checked against that table before a durable record is produced, so an illegal
transition is refused by name rather than applied and discovered later.

## Partition semantics

A partition here is a real event on a real path. @dcf-relay@ forwards bytes
between sites and the federation and can cut one named site off; doing so closes
that site's connections and refuses new ones, which is what a partition is.

- A site keeps its **local authority** while disconnected. It keeps its own
  epoch, it keeps working, and the federation never overrides that.
- A grant carries an explicit **survivability rule**. A grant that does not
  survive a partition is suspended while its grantee is partitioned, so an
  isolated site cannot exercise authority that was granted before the link was
  lost. Suspension is driven by an *observed* partition: an unknown link is not a
  partition, because the absence of evidence about a link is not evidence that
  the link is down.
- **No new federation authority is fabricated in isolation.** Delegated acts are
  refused unless the site composed them against the federation's current
  generation, so a message written before a partition cannot take effect after it.
- **Exclusive authority cannot be double-delegated across a partition.** When an
  exclusive grant's fate is undecided, a second exclusive grant over the same
  scope and selectors is refused.
- On reconnect the federation compares generations and history digests and
  produces a deterministic outcome. A genuine conflict is never resolved by
  preferring whichever side answered last: it is recorded as
  @conflict_unresolved@ and stays open until an operator resolves it explicitly,
  naming the authority the resolution is made under.

## Compatibility

Compatibility is versioned and capability-based rather than a product version
comparison, and it is evaluated against **staged windows**: each capability has a
range the federation offers, a generation from which it is in force, an optional
deprecation generation and an optional removal generation.

Every evaluation produces findings with codes, subjects and explanations, ordered
canonically, so the same declaration evaluated at the same generation always
produces the same report - including the order of the reasons. A refusal can be
reproduced exactly from the journal.

Two rules are worth stating explicitly because they are easy to get wrong:

- a zero deprecation or removal generation means the stage was **not scheduled**,
  not that it happened at generation zero;
- a site that declares no capabilities at all is refused rather than admitted
  with a reduced scope, because admitting it would be claiming a compatibility
  the federation cannot vouch for.

## Persistence and recovery

A store directory holds:

```
CURRENT                 atomically replaced; names the snapshot and the journal segment
state.<seq>.dcfsnap     an immutable snapshot of the committed state
journal.<first>.dcflog  an append-only segment of framed records
store.lock              an exclusive advisory lock, released even on a crash
```

A journal record is @u32 payload length | u32 CRC-32C | canonical payload@. A
snapshot is a header, the canonical state, a CRC and the canonical digest.

**The commit boundary is the flush.** @commit()@ returns only after the bytes
have reached storage: @fsync@ on POSIX, @FlushFileBuffers@ on Windows. The publish
boundary is separate and belongs to the caller: a committed entry is not visible
to readers until the engine has applied it. Submission is not completion, and
serialization is not durability.

**Compaction cannot supersede uncommitted state.** It snapshots exactly the
committed state, writes the snapshot through a temporary file with an atomic
replace, creates the new journal segment, and only then replaces @CURRENT@. A
crash at any point leaves either the previous pair (still consistent) or the new
pair; the orphan is removed on the next open and reported.

**Torn tail and interior corruption are distinguished, and only one is repaired.**

- Fewer remaining bytes than a frame header, or a run of zero bytes, is an
  unwritten region: the store truncates to the last complete record and reports
  exactly how many bytes were removed.
- A record whose declared length does not fit, or whose checksum does not match,
  or which does not decode, is **interior corruption**. The store refuses to open
  and never truncates through it.

An incomplete final record was never acknowledged, because @commit()@ does not
return until the record is complete and flushed. That is why removing it loses
nothing that was promised. There is one honest limitation: a bit flip in a
record's own length field can make a complete final record look incomplete, and
the store will then remove it and report the byte count. Nothing else can cause a
silent removal, because every complete record is checksummed and a checksum
failure is never repaired. The count is reported so an operator can see it.

Recovery replays the journal segment in order after loading the snapshot, and the
state it produces is the state the live process held - there is one code path for
both.

## Concurrency and ownership

There is exactly one mutator thread. Every state change in the process happens on
it, in submission order, so no lock protects authoritative state and no
read-modify-write race is possible by construction.

- Submitters place a command on a **bounded** queue and return; a submitter that
  arrives when the bound is reached is refused with @capacity_exhausted@.
- The mutator evaluates, makes the entry durable, applies it, and publishes an
  immutable snapshot.
- Readers take a consistent copy of the published snapshot and never block the
  mutator for longer than the copy takes.
- **Callbacks never run under a lock.** A callback may read state, submit more
  work, or throw.
- A callback that submits does not block: the command is parked and the mutator
  picks it up when the callback returns.
- A callback that submits *synchronously* is refused with a policy error naming
  the alternative, because the mutator would be waiting for itself. Refusing is
  the whole point: the failure mode of the alternative is a hang.
- The queue mutex and the publication mutex are never held at the same time, so
  there is no lock order to get wrong.
- Shutdown stops accepting, drains everything already accepted, stops the mutator
  and joins it. An accepted command is never dropped by a shutdown.

The audit this design answers, point by point: no read-lock to write-lock upgrade
anywhere, because there is no reader-writer escalation; no write lock held across
a call that can acquire the same state, because there is no state lock at all; no
mutex re-entry through callbacks, and re-entry is answered explicitly; no emission
while holding locks; no shutdown while holding a lock a worker needs; no worker
joined while holding state it requires; no reversed lock ordering; no stale
asynchronous completion, because there are no asynchronous completions outside the
single mutator; and no race between revocation, recovery and publication, because
all three happen on the same thread in a fixed order.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: @DCF_BUILD_TOOLS@, @DCF_BUILD_TESTS@, @DCF_BUILD_BENCHMARKS@,
@DCF_WARNINGS_AS_ERRORS@, @DCF_ENABLE_ASAN@, @DCF_ENABLE_UBSAN@.

First-party code is warning-free under MSVC @/W4 /WX /permissive-@ and under GCC
and Clang with @-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
-Wdouble-promotion -Wformat=2 -Werror@.

There is **no test timeout anywhere** - not in CTest, not in the CI workflow, not
in a wrapper. A test that never finishes is a defect to be diagnosed from
evidence.

## Installing and consuming

```
cmake --install build --prefix /some/prefix
cmake -S examples/consumer -B consumer-build -G Ninja -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build consumer-build
./consumer-build/dcf-consumer /tmp/consumer-store
```

The package is @DataCenterFederation@, the exported target is
@DataCenterFederation::dcf@, and the transitive dependencies (@Threads::Threads@,
and @ws2_32@ on Windows) are carried by the exported target and resolved by the
package config, so a consumer needs no platform-specific link flags of its own.

## The programs

@dcf-federationd@ hosts one federation: it owns the durable store, the runtime,
and the protocol that sites and operators speak.

@dcf-sited@ hosts one site. It owns its own local epoch and durable local state,
announces itself to a relay, registers, activates when told it has been admitted,
reports what it has accepted, and keeps working while the link is down.

@dcf-relay@ forwards bytes and can cut one site off. Its control port accepts
@BLOCK <token>@, @ALLOW <token>@, @STATUS@ and @QUIT@.

@dcfctl@ is the operator tool.

A complete session, using the ports a run actually printed:

```
$ dcf-federationd --store /srv/dcf --listen 127.0.0.1:0
federation 3f2ae1a62737868e8ec1651afe081ecc
listening 127.0.0.1:53411
generation 1 sequence 1
recovery reopened=0 replayed=0 torn_tail_bytes=0
READY

$ dcf-relay --listen 127.0.0.1:0 --upstream 127.0.0.1:53411 --control 127.0.0.1:0
relay-data 127.0.0.1:53412
relay-control 127.0.0.1:53413
upstream 127.0.0.1:53411
READY

$ dcf-sited --id 00000000000000010000000000000001 --name alpha \
            --relay 127.0.0.1:53412 --capabilities dcf.membership \
            --local-state /srv/alpha.state --control 127.0.0.1:0
site 00000000000000010000000000000001 name alpha
endpoint 127.0.0.1:53412 (through relay)
READY
control 127.0.0.1:53414

$ dcfctl --endpoint 127.0.0.1:53411 window --capability dcf.membership --offered 1.0..3.0 --from 1
{"verb":"window","outcome":"none","generation":3,"membership_generation":0}

$ dcfctl --endpoint 127.0.0.1:53411 validate --site 00000000000000010000000000000001
{"verb":"validate","outcome":"none","generation":4,"membership_generation":1}

$ dcfctl --endpoint 127.0.0.1:53411 admit --site 00000000000000010000000000000001 --generation 2
{"verb":"admit","outcome":"none","generation":5,"membership_generation":3}

$ dcfctl --endpoint 127.0.0.1:53411 sites
[{"site":"00000000000000010000000000000001","display_name":"alpha","state":"active", ...}]
```

Cut the site off, keep working locally, heal, and read the reconciliation:

```
$ dcf-relay control: BLOCK 00000000000000010000000000000001
OK blocked 00000000000000010000000000000001

$ dcf-sited control: LOCAL-OP
OK local-op epoch 1

$ dcf-relay control: ALLOW 00000000000000010000000000000001
OK allowed 00000000000000010000000000000001

$ dcfctl --endpoint 127.0.0.1:53411 reconciliations
{"total":1,"reconciliations":[{"...","outcome":"site_adopts_federation", ...}]}
```

## Library use

```cpp
#include "dcf/engine.hpp"
#include "dcf/store.hpp"

dcf::FederationEngine engine(dcf::Limits{}, clock);
engine.replay(dcf::genesis_entry(federation, dcf::UnixMillis{now}));

dcf::Command command;
command.payload = dcf::RegisterSiteCommand{site, "alpha", declaration};
command.origin = site;

const auto plan = engine.evaluate(command);   // decide, change nothing
if (plan && plan.value().persists) {
  store.commit(plan.value().entry);           // make it durable
  engine.commit(plan.value());                // then make it visible
}
```

Deciding and committing are separate on purpose. A decision that is not durable is
never visible.

## Validation performed

Debug and Release on Windows 11 x64 with MSVC 19.44.35209 (Visual Studio 2022),
Ninja 1.13.2 and CMake 4.3.2:

| suite | cases | checks executed |
| --- | --- | --- |
| @unit_foundation@ | 21 | 118 |
| @unit_engine@ | 24 | 82 |
| @unit_store@ | 14 | 32 |
| @unit_runtime@ | 8 | 30 |
| @prop_adversarial@ | 6 | 16588 |
| @cluster_partition@ | 6 | 49 |
| **total** | **79** | **16899** |

All 79 cases and all 16899 checks pass in both configurations. Release on the
same machine with GCC 14.2.0 and with Clang 19.1.1 (MinGW-w64 UCRT) also builds
under the full warning set with no diagnostics and passes all 79 cases.

What the suites actually do:

- @unit_foundation@: identifiers, checked arithmetic, version windows, SHA-256
  against published vectors, CRC-32C against the published vector, UTF-8
  validation including overlong forms and surrogates, and the canonical codec's
  refusal of truncated, padded, oversized and non-canonical input.
- @unit_engine@: the lifecycle, explainable compatibility refusal, staged windows,
  delegation and revocation, exclusive-authority conflicts, stale generation
  fencing, idempotent replay, partition suspension and restoration, removal
  fencing, and every reconciliation outcome including @federation_regression@ and
  @conflict_unresolved@.
- @unit_store@: real close and reopen, a torn tail that is truncated and reported,
  an all-zero tail, interior corruption that is refused and left untouched, a
  complete final record with a bad checksum, compaction, writes after compaction,
  orphan removal, a missing snapshot, a damaged @CURRENT@, the exclusive lock,
  reordered entries, and a truncated snapshot.
- @unit_runtime@: concurrent submitters with concurrent readers, a callback that
  resubmits, a callback that throws, a synchronous submission from a callback, a
  bounded queue that says so, shutdown that does not drop accepted work, close and
  reopen, and compaction.
- @prop_adversarial@: 24 fixed seeds driving 60 random commands each, with the
  invariants checked after every single step (the generation never decreases, the
  sequence advances by one exactly when a record was written, the digest is a pure
  function of the state) and a full replay of the recorded entries from genesis
  reproducing the same state; a byte-by-byte mutation sweep of a real journal
  where every single-bit and single-byte change must either open to the *same*
  state or be refused with a defined error; 4000 mutated and truncated command
  encodings that must either decode to something that re-encodes to the bytes it
  came from or be refused with a defined error; absurd lengths, inverted ranges,
  non-canonical booleans, invalid UTF-8, duplicate identities, a path traversal
  attempt in a capability name, a store path beyond the Windows path limit, and 25
  rounds of open and close.
- @cluster_partition@: real operating-system processes over TCP - a federation, a
  relay, a site, and the installed CLI - covering registration, compatibility
  validation, admission, activation, a partition made by the relay closing the
  path, local operations that succeed while the site is cut off, a real reconnect
  and reconciliation, removal during a partition with fencing on reconnect, a
  federation killed without warning and restarted on the same store with an
  identical authority digest, an exclusive delegation that cannot be double
  granted across a partition, and the installed @dcfctl@ driving the federation.

## Benchmarks

Measured on Windows 11 x64, 16 hardware threads, Release build with MSVC 19.44,
by @bench_federation@. Every figure is a measurement taken by the run that
produced it; nothing is estimated. Durable operations are reported separately
because they include a flush to storage and are not comparable with in-memory
evaluation.

| measurement | operations | total | per operation |
| --- | --- | --- | --- |
| compatibility evaluation, 1000 sites | 4000 | 3.78 ms | 0.945 us |
| reconciliation decision, 1000 sites | 1000 | 0.03 ms | 0.033 us |
| canonical authority digest, 1000 sites | 8 | 4.37 ms | 546.7 us |
| published state copy, 1000 sites | 8 | 2.05 ms | 256.3 us |
| compatibility evaluation, 10000 sites | 40000 | 34.39 ms | 0.860 us |
| reconciliation decision, 10000 sites | 10000 | 0.21 ms | 0.021 us |
| canonical authority digest, 10000 sites | 8 | 40.41 ms | 5051.8 us |
| published state copy, 10000 sites | 8 | 32.93 ms | 4116.3 us |
| durable commit (write and flush) | 500 | 484.09 ms | 968.2 us |
| compaction (snapshot and rotate segment) | 1 | 9.20 ms | 9196.2 us |

Two things follow plainly from the numbers, and they are stated here rather than
left for a reader to discover:

- the two decisions this boundary exists to make - is this site compatible, and
  what does this reconnect mean - cost about a microsecond and are effectively
  free at any realistic member count;
- the whole-state operations are not free. Producing the canonical authority
  digest of a 10000-site federation takes about 5 ms, and copying the published
  state takes about 4 ms. Both are linear in the size of the state, and at that
  size they dominate everything else. A deployment that recomputes the authority
  digest on every command at that scale will spend its time there. Measuring them
  was the point of measuring them.

## Platform support and limitations

- **Windows x64 with MSVC** is the configuration that was built, tested,
  installed, consumed and benchmarked on this machine, in Debug and Release.
- **POSIX** code paths are implemented for Linux and macOS, and the CI workflow
  builds and tests them with GCC and Clang in Debug and Release, with sanitizer
  jobs. They were **not** built or run on the machine that produced this release,
  and nothing here claims otherwise.
- Windows long-path handling is real but narrow: the store creates its own
  directories and opens, sizes, lists, removes and atomically replaces its own
  files through the platform with the long-path prefix applied, and a store root
  beyond the legacy limit is covered by a test. Other programs on the same machine
  may still not handle such a path.
- Windows has no operation that flushes a directory entry. The atomic replace is
  the durability boundary for the name; the file contents are flushed before it.
- GCC 14 at @-O3@ reports @-Wfree-nonheap-object@ inside libstdc++'s
  @new_allocator.h@ for two translation units that use @std::variant@ and
  @std::vector@ in the ordinary way. The diagnostic names a pointer it cannot
  identify, disappears at @-O2@ and below, and points into a standard library
  header. It is suppressed for exactly those two files and nothing else; the
  reasoning is written next to the suppression in the build file.
- The multiprocess suite runs real processes over the loopback interface on one
  machine. That is genuine multiprocess behaviour; it is not a multi-machine
  deployment test, and no such test is claimed.
- There are no Windows sanitizer results, because MSVC has no equivalent of ASan
  plus UBSan for this code. The Linux sanitizer jobs cover it.

## Relationship to adjacent boundaries

Data Center Federation composes sites into a federation. It consumes, but never
owns:

- **Accelerated Systems Infrastructure (ASI)** for accelerator execution, memory,
  serving, scheduling, state and capability. A federation records a capability
  name and a version range; it does not schedule anything.
- **Distributed Fabric Infrastructure (DFI)** for topology, paths, transport,
  congestion, failure and recovery. A federation records connectivity as a link
  state on a membership record; it does not choose a path.
- **Earlier DCCP boundaries** for facility identity, topology, assets, capacity,
  power, cooling, lifecycle, policy, tenancy, failure and recovery, observability
  and economics. A federation references a policy domain; it does not evaluate
  policy.

The invariant is that this repository owns exactly one thing and consumes
neighbouring truth explicitly. Where an integration exists, it exists as a typed
reference that can be compared and hashed, not as a door into another runtime.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
