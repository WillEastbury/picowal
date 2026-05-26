# Lookup filters and many-to-many mapping queries

Picowal exposes the shared lookup/relation semantics through bounded query helpers.

## Filtered lookup

Use `query_build_lookup_filter(...)` to build a safe query for lookup option lists:

```c
char q[160];
query_build_lookup_filter(
    q, sizeof q,
    "todos", "name",
    "status", QOP_NE, "completed",
    "id", current_id);
```

This produces:

```text
S:name
F:todos
W:status|!=|completed
W:id|!=|<current>
```

The helper validates pack/field tokens and writes into caller-owned buffers only.

## Many-to-many mapping cards

Use `query_build_many_to_many_map(...)` to query a hidden mapping pack:

```c
char q[128];
query_build_many_to_many_map(
    q, sizeof q,
    "investigator_capability",
    "investigator", investigator_id,
    "capability");
```

This produces a query selecting capability IDs from the mapping pack for one
investigator. Callers can then resolve the target IDs through the existing card
read or lookup path.

## Constraints

- no heap allocation;
- fixed caller-provided buffers;
- no flash writes;
- validates metadata tokens before writing query text;
- uses existing `W:` predicates (`==`, `!=`, `IN`, `NI`, etc.).
