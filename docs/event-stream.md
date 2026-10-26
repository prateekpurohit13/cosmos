# Cosmos Event Stream (contract v1)

Status: **U3.1 implemented; engine instrumentation is U3.2.** The header is
`include/cosmos/event_sink.hpp`; the contract tests are `tests/test_event_sink.cpp`.

This document is the contract for the inspection event stream described in
[`ui-design.md`](ui-design.md) §3.4, §10.2 and §10.3. The stream is deliberately **not** the
canonical `FaultTrace`: replay, minimization and `trace_hash` consume `include/cosmos/trace.hpp`
only (D5). The event stream exists to explain what a universe did across the scheduler, network,
storage, memory, clock, randomness, application and operator domains. Recording an event never
changes simulation semantics.

Nothing in the engine produces these events yet. U3.1 freezes the envelope, sequence, gap and
wire format so U3.2 can add producers without redesigning the contract.

## 1. Boundaries

- **Read-only observation.** The stream is captured by a universe and consumed by an artifact
  reader or UI. It is not a control channel.
- **Not replay identity.** No event is written into `FaultTrace`, no event changes `trace_hash`,
  and no replay path reads this stream.
- **Not a live UI call.** The UI server remains a separate process (D4). Recording must not call
  the UI, block, perform I/O or allocate.
- **Numeric payloads in v1.** Envelope and payload are fixed-width scalars. Application strings,
  spans with names and redaction arrive with the U3.5 telemetry API and may require a new stream
  version.

## 2. Envelope

Every event carries:

| Field | In memory | Meaning |
|---|---|---|
| `universe` | `uint64_t` | Universe seed, not its campaign index. The sink stamps this. |
| `seq` | `uint64_t` | Per-universe event sequence, starting at `0` and contiguous. The sink stamps this. `(universe, seq)` is the event identity. |
| `at` | `Time` (`int64_t` ns) | Virtual time. This is the simulation axis. |
| `wall_ns` | `int64_t` | Optional host wall-clock metadata. Never an ordering key. |
| `has_wall` | `bool` | Whether `wall_ns` was captured. Encoded as one byte. |
| `kind` | `EventKind` (`uint16_t`) | One of the append-only kinds in §5. `none` is not recordable. |
| `node` | `NodeId` (`uint32_t`) | Node the event belongs to, or `kNoNode` (`0xFFFFFFFF`). Node `0` is valid. |
| `task` | `uint64_t` | Scheduler task, or `kNoTask` (`0`). |
| `correlation_id` | `uint64_t` | Opaque correlation identity, or `kNoCorrelation` (`0`). A fault-related event may carry the canonical trace decision `seq` here, but the two sequence namespaces are distinct. |
| `payload` | `EventPayload` | Five fixed-width slots; meaning is fixed by `kind` (§6). |

Wall time is shown by a UI only as host metadata. It must never be used to order, filter or
align events. A sink must be given its universe seed before the first record; an unseeded sink
rejects records instead of stamping them with a misleading universe `0`.

## 3. Capture, sequence and gaps

`cosmos::EventSink` is a fixed-capacity ring:

- The buffer is allocated once in the constructor, **before** the universe starts and before the
  wrapped allocation path is current. `record()` only stamps `universe`/`seq` and assigns into the
  preallocated slot. It does not allocate, lock, perform I/O or call the UI.
- `capacity == 0` means disabled. A disabled sink discards every call and assigns no sequence.
- An enabled full ring **drops the oldest event** and keeps the newest `capacity` events. This is
  the flight-recorder policy from `ui-design.md` §7.3: the most recent evidence is the evidence a
  failure usually needs.
- An enabled sink rejects a record with an unknown kind, before its seed was set, or after its
  sequence space is exhausted. A rejected call gets no sequence and is counted by `rejected()`;
  it is a producer contract violation, not an evicted event. v1 assigns sequences
  `0 .. UINT64_MAX-1`; `UINT64_MAX` is reserved so `dropped + count` and `first_seq + index`
  cannot overflow.
- `next_seq()` is the number of **accepted** events; while the sequence space is not exhausted,
  it is also the sequence the next accepted event will receive. `first_seq()` is the oldest
  retained sequence, or `next_seq()` when empty. `dropped()` counts evicted events, never
  retained ones; `rejected()` counts refused calls.
- `record()` asserts that the call was accepted, so a producer bug fails loudly in a debug build.
  `try_record()` is the non-asserting form: it returns false (and increments `rejected()`) when an
  enabled sink refuses the event, and true for a disabled sink.
- Moving a sink leaves the source disabled and empty; record on a moved-from sink is a no-op. The
  sink is not copyable, because a copy would allocate a second buffer.
- `operator[]` is a logical-order accessor. An out-of-range index returns a default event
  (`kind == none`) in release builds; check `size()` and never treat that value as evidence.
- `gap()` returns `EventGap{universe, count, first_seq}` when anything was evicted. In v1 the
  capture starts at sequence zero, so the missing prefix is `[0, first_seq)` and `count` equals
  the number of missing events. `EventStream::gap()` derives both fields from the retained
  entries, so a hand-built inconsistent stream cannot render a gap whose count and first sequence
  disagree; the encoder still refuses such a stream.
- One writer only. A universe belongs to one OS thread; the sink does not lock.

A consumer **must** render the gap. Presenting the retained window as the whole universe would
claim evidence that was lost. A gap is not a synthetic event in the ring: the explicit
`dropped`/`first_seq` metadata is the marker, so it cannot consume capacity or collide with a real
sequence. A consumer **must** also surface `rejected > 0` as a producer-contract warning, distinct
from the eviction gap: a rejected call never received a sequence, so it is covered by neither the
gap nor the entry list.

## 4. Wire format (version 1)

Little-endian, fixed widths, no padding. The source byte distinguishes this stream from the
canonical trace (`trace` source is `1`; source `2` is unallocated).

`inline constexpr uint8_t kEventStreamSource = 3;`
`inline constexpr uint8_t kEventStreamVersion = 1;`

### Header — 34 bytes

| Offset | Width | Field |
|---|---|---|
| 0 | 1 | source = `3` |
| 1 | 1 | version = `1` |
| 2 | 8 | `universe` |
| 10 | 8 | `dropped` |
| 18 | 8 | `rejected` |
| 26 | 8 | `count` (retained entries) |

### Entry — 87 bytes

| Offset | Width | Field |
|---|---|---|
| 0 | 8 | `seq` |
| 8 | 8 | `at.ns` (signed, two's complement) |
| 16 | 1 | `has_wall` (`0` or `1`) |
| 17 | 8 | `wall_ns` (signed) |
| 25 | 2 | `kind` |
| 27 | 4 | `node` |
| 31 | 8 | `task` |
| 39 | 8 | `correlation_id` |
| 47 | 8 | `payload.subject` |
| 55 | 8 | `payload.related` |
| 63 | 8 | `payload.value` |
| 71 | 8 | `payload.extra` |
| 79 | 8 | `payload.amount_ns` (signed) |

The universe is written once in the header and stamped onto each decoded event. Entry sequences
must be exactly `dropped + index`; a non-contiguous or reordered stream is malformed, not a
different valid capture. Zero entries with a non-zero `dropped` is also malformed: a non-empty
ring never evicts its last entry, so that combination cannot be a v1 capture. `rejected` counts
calls that never received a sequence and therefore does not affect the entry sequence.

`encode_events()` writes the retained window and returns `std::expected<std::string,
EventStreamError>`; it rejects inconsistent hand-built streams instead of asserting in release.
`decode_events()` returns `EventStream` with `universe`, `dropped`, `rejected` and `entries`.
Encoding is off the hot path and may allocate. Capacity is capture policy, not content, so it is
not encoded (the same rule as the canonical trace).

An emitted empty stream is a complete capture of zero events, but it does not by itself prove a
sink was attached. An artifact writer must record presence separately, as it does for the
canonical trace (`trace.present`). `encode_events(const EventSink&)` refuses an enabled sink that
was never seeded; a disabled sink still encodes a header-only empty stream.

A stream with zero entries and a non-zero `dropped` is refused by both the encoder and the decoder;
the v1 ring always retains at least one event once it has evicted anything. The encoder also
refuses a hand-built entry whose `has_wall` is false but `wall_ns` is non-zero. `record()` clears
`wall_ns` when `has_wall` is false, so a captured event is canonical before it reaches the wire.

Readers reject, rather than guess at:

- a truncated header or entry (`Truncated`);
- the wrong source (`BadSource`) or an unknown version (`BadVersion`);
- trailing bytes (`TrailingBytes`);
- an unknown kind, a bad wall-presence byte, a non-zero `wall_ns` when `has_wall` is false, a
  non-contiguous sequence, a zero-entry stream with a non-zero gap, or a sequence range
  overflowing `uint64_t` (`BadValue`).

An additive change to the header, entry layout or payload interpretation requires a new stream
version and a reader that rejects the versions it does not know. There are no silent field
renames or truncations.

## 5. Event taxonomy

Kinds are append-only. A new kind takes the next value before `_Count`; an existing value is
never renumbered or reused, because artifacts and saved filters key on the token. Domains group
kinds into timeline lanes.

| Domain | Kinds |
|---|---|
| `scheduler` | `spawn`, `run`, `block`, `wake`, `exit`, `yield`, `mutex_lock`, `mutex_unlock`, `mutex_contend`, `cond_wait`, `cond_signal`, `cond_broadcast`, `join`, `detach`, `timer_arm`, `timer_fire`, `timer_cancel`, `deadlock`, `choice` |
| `network` | `socket`, `bind`, `listen`, `connect`, `accept`, `close`, `send`, `recv`, `enqueue`, `deliver`, `drop`, `delay`, `reorder`, `corrupt`, `endpoint_state`, `partition`, `heal` |
| `storage` | `open`, `read`, `write`, `fsync`, `fdatasync`, `pwrite`, `writev`, `ftruncate`, `dirty`, `durable`, `torn`, `rollback`, `degraded` |
| `memory` | `alloc`, `free`, `realloc`, `canary`, `leak` |
| `clock` | `gettime`, `gettimeofday`, `sleep`, `step` |
| `random` | `draw` |
| `application` | `always`, `sometimes`, `reachable`, `request`, `response`, `counter`, `gauge`, `span`, `annotation` |
| `operator` | `command`, `breakpoint_hit` |

Full tokens are the domain and kind joined by a dot, for example `scheduler.spawn` or
`storage.fsync`; a compile-time check enforces the `<domain>.<kind>` prefix. `name_of(EventKind)`
returns the token; `domain_of()` returns the lane.

Fault decisions are deliberately not mirrored here. They remain in the canonical `FaultTrace` and
are joined to this stream by the UI through the `correlation_id` convention in §6. Node lifecycle
(crash/reboot/pause/resume) is also not in v1; U3.2 appends those kinds before recording node
state, following the same append-only rule.

`name_of(EventKind)` returns `"?"` and `domain_of()` returns `EventDomain::_Count` for a value that
is not a declared kind (`none`, `_Count` or an out-of-range cast). A UI must treat both as unknown
rather than as a lane.

## 6. Payload slots

`EventPayload` is five fixed-width slots so `record()` cannot allocate:

| Slot | Type | Intended use |
|---|---|---|
| `subject` | `uint64_t` | Primary object: task, fd, allocation, packet, endpoint, span. |
| `related` | `uint64_t` | Secondary object: peer, parent, waiter, target. |
| `value` | `uint64_t` | Size, count, state, errno or outcome code. |
| `extra` | `uint64_t` | Kind-specific second scalar: a length when `value` is an offset, a transferred count when `value` is requested, and so on. |
| `amount_ns` | `int64_t` | Duration, deadline or offset. |

The exact slot meaning is fixed by `kind`. U3.1 fixes the slots and the envelope; U3.2 records the
per-kind meaning alongside each producer and extends this section with the concrete producer
table. Producers must not overload a slot with a different meaning for the same kind. A kind that
carries a state, errno or outcome in `value` must publish an append-only code enum with the kind;
the UI maps an unknown code to a visible "unknown code" label rather than guessing. The fifth slot
exists so a kind needing two scalars (offset+length, requested+transferred, old+new position) does
not have to overload one.

Correlation is what makes a causal story visible. A `network.send` with `correlation_id = C`
should be followed by the `network.drop`, `network.deliver` or receiving task event with the same
`C`. The stream does not claim causality the producer did not record.

## 7. Artifact placement (planned for U3.2)

U3.1 does not write an artifact; it freezes the shape U3.2 will use:

- `events/<index>.bin` holds the version-1 bytes for one universe, alongside `traces/<index>.bin`.
- `universes/<index>.json` and the `universes.ndjson` row gain an additive `events` object:
  `present` (bool), `path` (relative path or empty), `version`, `dropped`, `rejected`, `seeded`.
- Absence is `present: false`, never an empty success. A present file with a gap or rejected calls
  carries those counts in the metadata so a view can label it without decoding first.
- The artifact writer must not write an empty header for a sink that was never attached. An
  attached, seeded sink with zero accepted events may write an empty stream; `present` still
  distinguishes it from no instrumentation.

This is a planned additive extension to schema v1 (D11); no reader may infer presence from a
missing file alone.

## 8. Non-goals for v1

- No live event streaming from a running universe.
- No scheduler/network/storage/memory instrumentation until U3.2.
- No state snapshots, breakpoints or interactive control.
- No application strings, names or payload hexdumps until U3.5; redaction is part of that API.
- No change to the canonical trace, `trace_hash`, replay or minimization.
- No wall-clock ordering and no synthetic events to hide a gap.

## 9. Guardrails

| Risk | Guardrail |
|---|---|
| Telemetry allocates inside a wrapper | Construct the sink before the universe starts; `record()` is a preallocated assignment, and the static-libstdc++ test compares cumulative allocation counters, not just live blocks. |
| A lost event looks like an absent event | Drop-oldest is explicit through `dropped()`, `first_seq()` and `gap()`; rejected calls are explicit through `rejected()`; the wire header carries both. |
| A default-constructed event poisons the artifact | `kind == none`, an unseeded sink and sequence exhaustion are rejected and counted, never written; the encoder also refuses an inconsistent hand-built stream in release. |
| A huge capture floods memory | Capacity is chosen at construction and never grows; an oversized capture is a gap, not an OOM on the hot path. |
| A malformed artifact is shown as truth | The decoder rejects truncation, unknown versions, bad kinds, non-contiguous sequences and trailing bytes. |
| UI telemetry changes replay | The event stream is separate from `FaultTrace` and `trace_hash`; no replay path includes it. |
| Wall time leaks onto the simulation axis | `wall_ns` is optional metadata and is documented as never an ordering key. |
| Version drift | One source byte and one version byte; unknown versions are rejected rather than guessed. |
| Recording blocks a universe | No I/O, no locks and no UI call from `record()`; instrumentation must preserve that property in U3.2. |
