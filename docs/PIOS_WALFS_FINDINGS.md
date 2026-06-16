# PIOS WALFS/Picowal adapter findings

This note records failures found while wiring the PIOS PicoScript webserver
prototype to Picowal-backed cards.

The upstream Picowal C API contract remains straightforward:

- `put(pack, card, value)` must make the latest value immediately readable by
  `get(pack, card)`.
- Updating an existing card must not delete the old value before the replacement
  is durable.
- `put` must not report success until payload, metadata/index state, and commit
  state agree.
- `list(pack)` must not hide cards that were successfully created.

## Live PIOS symptoms

On PIOS, `db put`/`pixe put` reported success, but follow-up `db get` either
failed or returned stale/binary data. `walfs verify` then reported combinations
of:

- `open_tx=yes`
- CRC/header scan failures
- cards missing from `db list` despite a successful write response

This surfaced when the live port-81 PicoScript webserver attempted to preload:

- bytecode program card
- static HTML card
- JSON API card

Serving directly from those bad reads produced garbage bytes, so PIOS now treats
card-loaded web assets as optional and falls back to embedded bytecode/content
when the preloaded payload fails basic validation.

## Root causes identified in the PIOS adapter

1. **Live WAL data index was stale after writes.** PIOS WALFS rebuilds its
   `data_index` at mount, but `walfs_write()` appended `RECORD_DATA` records
   without adding them to the in-memory index. Immediate reads used the old index
   and could return stale data or miss the record until reboot/recovery.

2. **Overwrite used delete-then-create.** The PIOS `picowal_db_put()` adapter
   deleted an existing record path before creating/writing the replacement. If
   create/write/commit failed after the delete, the old card was gone and an
   open transaction could remain. Picowal-style update should be all-or-nothing:
   update in place or write a replacement transaction and publish it only after
   commit.

3. **Success was too optimistic.** `db put` reported success while the WAL could
   still be left in a state that `walfs verify` flagged as open/corrupt. Any WAL
   backend should fail closed: no user-visible success unless the commit record
   and read index are coherent.

## Upstream invariants to preserve

The Picowal host filesystem backend already uses write+fsync+rename, which is the
right shape: the old card survives until the new file is fully written and
renamed into place. The host smoke test now also asserts:

- create-only duplicate detection
- update/read-after-write returns the replacement value
- `list` still reports the updated card
- delete removes the card

Any future WAL/SD adapter should pass the same behavioral invariant, even if its
on-disk format is append-only rather than rename-based.
