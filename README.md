# group_label

A DuckDB extension providing two graph primitives:

- **`connected_components`** — Union-Find over an explicit edge list. Works on any graph built from SQL predicates: same-row, containment, proximity, color match, arbitrary join.
- **`label_groups`** — Spatial connected components for pixel groups on a grid. Two groups are connected when any pixel from one is within a configurable distance of any pixel from the other.

---

## Installation

```bash
git clone --recurse-submodules https://github.com/yourname/group_label.git
cd group_label
GEN=ninja make
```

```sql
LOAD 'build/release/extension/group_label/group_label.duckdb_extension';
```

---

## `connected_components`

Pure Union-Find over an explicit edge list. Use this when connectivity depends on something other than pixel proximity: deduplication, clustering, network reachability, same-row, containment, shared property, arbitrary join.

```sql
→ TABLE(node_id BIGINT, component_id BIGINT, component_size INTEGER)
```

`component_id` is always one of the input `node_id` values (the Union-Find root). Only equality matters — the specific value is arbitrary. `component_size` is the number of nodes in that component.

### Calling conventions

**Array form** — pass pre-aggregated lists. Works with everything including temp tables and CTEs.

```sql
-- explicit node list + edges
connected_components(node_ids BIGINT[], edge_a BIGINT[], edge_b BIGINT[])

-- nodes inferred from edge endpoints (isolated nodes excluded)
connected_components(edge_a BIGINT[], edge_b BIGINT[])
```

**`$variable` form** — pass variable names prefixed with `$`. DuckDB variables live on the same session context so temp tables, CTEs, and subquery results are all accessible via `SET VARIABLE`. No deadlock, no visibility issues.

```sql
-- nodes inferred
connected_components('$ea', '$eb')

-- explicit node list
connected_components('$node_ids', '$ea', '$eb')
```

**Table-name form** — pass table and column names as string literals. Runs against the live database, so only **persistent tables** are visible (temp tables are session-scoped).

```sql
-- nodes inferred from edge endpoints
connected_components('edge_table', 'src_col', 'dst_col')

-- explicit node table (captures isolated nodes)
connected_components('node_table', 'node_col', 'edge_table', 'src_col', 'dst_col')
```

### Which form to use

| Situation | Form |
|---|---|
| Persistent `edges` table | table-name |
| Temp table / CTE / subquery result | `$variable` |
| Already have lists in hand | array |
| Want isolated nodes | 3-arg array, 3-arg `$variable`, or 5-arg table-name |

### `$variable` pattern

```sql
-- Works with temp tables, CTEs, filtered subqueries — anything
SET VARIABLE ea = (SELECT list(src ORDER BY src) FROM my_edges WHERE active);
SET VARIABLE eb = (SELECT list(dst ORDER BY src) FROM my_edges WHERE active);

-- nodes inferred from edge endpoints
SELECT * FROM connected_components('$ea', '$eb');

-- or with an explicit node list to capture isolated nodes
SET VARIABLE node_ids = (SELECT list(id) FROM my_nodes);
SELECT * FROM connected_components('$node_ids', '$ea', '$eb');
```

Variables are expanded at bind time from `ClientConfig::user_variables` on the same session context — no additional query execution, no locking issues.

### Examples

**Persistent edges table:**
```sql
SELECT * FROM connected_components('transactions', 'sender_id', 'receiver_id');
```

**Same-row connectivity** — objects sharing a minimum y coordinate:
```sql
SET VARIABLE nids = (SELECT list(id::BIGINT ORDER BY id) FROM objects);
SET VARIABLE ea   = (
    SELECT list(a.id::BIGINT ORDER BY a.id, b.id)
    FROM objects a JOIN objects b ON a.min_y = b.min_y AND a.id < b.id
);
SET VARIABLE eb   = (
    SELECT list(b.id::BIGINT ORDER BY a.id, b.id)
    FROM objects a JOIN objects b ON a.min_y = b.min_y AND a.id < b.id
);
SELECT * FROM connected_components('$nids', '$ea', '$eb');
```

**Containment** — is object B inside object A's bounding box?
```sql
SET VARIABLE ea = (
    SELECT list(a.id::BIGINT) FROM boxes a JOIN boxes b
    ON b.x0>=a.x0 AND b.x1<=a.x1 AND b.y0>=a.y0 AND b.y1<=a.y1 AND a.id!=b.id
);
SET VARIABLE eb = (
    SELECT list(b.id::BIGINT) FROM boxes a JOIN boxes b
    ON b.x0>=a.x0 AND b.x1<=a.x1 AND b.y0>=a.y0 AND b.y1<=a.y1 AND a.id!=b.id
);
SELECT * FROM connected_components('$ea', '$eb');
```

---

## `label_groups`

Spatial connected components over pixel groups on a grid. Two groups are connected when any pixel from one is within `distance` of any pixel from the other using a configurable metric.

```sql
label_groups(
    ids      BIGINT[],        -- group identifiers
    pixels   INTEGER[][],     -- pixel_id arrays per group (pixel_id = y * grid_w + x)
    grid_w   INTEGER,         -- grid width for decoding pixel_id → (x, y)
    distance DOUBLE  = 1.0,   -- max connecting distance
    metric   VARCHAR = 'L1',  -- 'L1' | 'L2' | 'Linf'
    grid_ids BIGINT[] = NULL  -- optional: grid isolation for mass labeling
) → TABLE(group_id BIGINT, component_id BIGINT, component_size INTEGER)
```

The last three parameters are optional — call with 3, 4, 5, or 6 arguments.

**Distance reference:**

| distance | metric | equivalent to |
|---|---|---|
| 1.0 | L1 | 4-connectivity |
| 1.0 | Linf | 8-connectivity |
| 1.415 | L2 | 8-connectivity |
| N | any | groups within N pixels |

**`grid_ids`:** groups with different non-zero `grid_ids` are never connected regardless of distance. `grid_id=0` means unconstrained (connects across all grids). Always use non-zero grid IDs for real isolation — zero is a special sentinel.

### Passing data to `label_groups`

`label_groups` only accepts literal arrays as arguments (DuckDB table function constraint). Use `SET VARIABLE` to pass dynamically-built data:

```sql
SET VARIABLE ids    = (SELECT list(group_id::BIGINT ORDER BY group_id) FROM groups);
SET VARIABLE pixels = (SELECT list(pixel_array     ORDER BY group_id) FROM groups);
SET VARIABLE gids   = (SELECT list(grid_id::BIGINT  ORDER BY group_id) FROM groups);

SELECT * FROM label_groups(
    getvariable('ids'),
    getvariable('pixels'),
    30, 1.0, 'L1',
    getvariable('gids')
);
```

### Examples

**Standard 4-connectivity pixel labeling** — each pixel is its own group:
```sql
SET VARIABLE ids = (SELECT list(pixel_id::BIGINT) FROM pixels WHERE example_id=0 AND c!=0);
SET VARIABLE px  = (SELECT list([pixel_id::INTEGER]) FROM pixels WHERE example_id=0 AND c!=0);

SELECT * FROM label_groups(getvariable('ids'), getvariable('px'), 30);
```

**Object-level labeling** — which pixel groups are touching?
```sql
SET VARIABLE ids = (
    SELECT list(floodfill_4_id::BIGINT ORDER BY floodfill_4_id) FROM red_objects
);
SET VARIABLE px = (
    SELECT list(pixel_list ORDER BY floodfill_4_id) FROM red_objects
);

SELECT lg.group_id, lg.component_id, lg.component_size
FROM label_groups(getvariable('ids'), getvariable('px'), 30, 1.0, 'L1') lg
WHERE lg.component_size > 1;
```

**Mass labeling — entire dataset in one call:**
```sql
-- grid_id encodes puzzle + example identity; must be non-zero
SET VARIABLE ids = (
    SELECT list(
        (puzzle_idx * 1000000 + example_idx * 1000 + floodfill_4_id)::BIGINT
        ORDER BY puzzle_idx, example_idx, floodfill_4_id
    ) FROM groups
);
SET VARIABLE px = (
    SELECT list(pixel_list ORDER BY puzzle_idx, example_idx, floodfill_4_id) FROM groups
);
SET VARIABLE gids = (
    SELECT list(
        ((puzzle_idx + 1) * 1000 + example_idx + 1)::BIGINT
        ORDER BY puzzle_idx, example_idx, floodfill_4_id
    ) FROM groups
);

SELECT * FROM label_groups(
    getvariable('ids'), getvariable('px'),
    30, 1.0, 'L1',
    getvariable('gids')
);
```

Groups from different grids never merge. This mirrors the mega-image encoding trick but stays entirely inside DuckDB — no Python, no scipy, no numpy round-trip.

---

## Performance

**`label_groups` proximity check** (`groups_within_distance`) is two-phase:

Phase 1: bounding-box pre-filter — O(1). If the bounding boxes of two groups are more than D apart on any axis, skip without touching any pixels.

Phase 2: for each pixel in the smaller group, binary-search the y-sorted pixel array of the larger group to find only the y-band `[y-D, y+D]`, then scan that band. For sparse groups and small D the y-band is typically 0–5 pixels even when groups have hundreds of pixels, reducing the inner loop from O(n_a × n_b) to O(n_a × |y_band|).

Early exit on first connecting pair.

**Group-pair loop** is O(n²) over the number of groups. The bounding-box filter eliminates most pairs in practice, but for very large group counts a spatial index (sweep line) would improve the worst case.

**`connected_components`** is O(n α(n)) Union-Find with path halving and union by rank, where α is the inverse Ackermann function — effectively O(n) for any realistic input.

**vs mega-image (arcducklib):** for standard pixel-level labeling across the full ARC dataset, `skimage.measure.label` on a packed numpy array is faster — it uses a two-pass O(N) C algorithm with SIMD. Use the mega-image approach for raw pixel labeling at scale. Use `label_groups` for generalized object-level connectivity (configurable distance, non-spatial predicates, mass labeling across grids) that arcducklib cannot express.

---

## Running the tests

```bash
make test
```

## License

MIT