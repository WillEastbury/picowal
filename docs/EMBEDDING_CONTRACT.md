# Picowal embedding contract

Picowal can be embedded inside a higher-level server process, but the embedding
surface should stay smaller than the appliance HTTP/UI surface. The host should
own request parsing, authentication, TLS, and presentation; Picowal should expose
storage primitives with explicit durability and recovery state.

This contract captures the production requirements found while embedding
Picowal-backed storage in a directly served site.

## Storage scope

- The embeddable API should target user data on disk-backed packs by default.
- System packs and flash-backed appliance metadata should not be exposed through
  the generic embedding API unless the caller opts in through a separate,
  explicit administrative path.
- The API should return a distinct "not ready" result when disk storage is not
  mounted or initialized, rather than silently falling back to another tier.

## Single-writer ownership

- A mounted volume has exactly one writer.
- Startup should fail clearly if another process already owns the volume.
- Read-only inspection tools may exist, but they must not share the write path
  or mutate recovery state while the writer is active.

## Quiesce for snapshots

A host that snapshots the underlying volume needs an explicit quiesce protocol:

1. Stop accepting new writes.
2. Drain in-flight writes.
3. Flush durable state and expose a stable snapshot point.
4. Keep reads available when possible.
5. Resume writes or fail closed if resume cannot be proven safe.

The API should make quiesce state observable so operators can distinguish
"draining", "quiesced", "resumed", and "failed".

## Capacity and compaction signals

Embedding hosts need cheap capacity signals without parsing internal files:

- volume bytes
- used bytes
- free bytes
- high-water mark
- compaction/reclaim status, when available

These should be exposed as integer counters or gauges so a host can translate
them into health endpoints, alerts, or UI warnings.

## Recovery and integrity state

After opening a volume, the API should expose the last recovery result:

- status code
- records scanned
- records recovered
- corrupt/truncated record counts
- recovered write offset
- volume size

The host can then report whether storage is ready, degraded, or requires manual
intervention without scraping logs.

## Error shape

The C API should avoid collapsing storage outcomes into a single boolean. Prefer
a small result enum that distinguishes at least:

- ok
- not found
- not ready
- invalid input
- no space
- busy/quiescing
- integrity error
- I/O error

That keeps hosts from turning storage faults into success-shaped fallbacks.
