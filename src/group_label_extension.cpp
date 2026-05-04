#define DUCKDB_EXTENSION_MAIN

#include "group_label_extension.hpp"
#include "group_label_core.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_config.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Shared result emission
// ─────────────────────────────────────────────────────────────────────────────

struct ResultState : public GlobalTableFunctionState {
    std::vector<group_label::UnionFind::Result> rows;
    idx_t offset = 0;
    idx_t MaxThreads() const override { return 1; }
};

static void emit_results(DataChunk &output, ResultState &state) {
    idx_t count = std::min(
        (idx_t)(state.rows.size() - state.offset),
        (idx_t)STANDARD_VECTOR_SIZE);
    output.SetCardinality(count);
    for (idx_t i = 0; i < count; ++i) {
        const auto &r = state.rows[state.offset + i];
        output.data[0].SetValue(i, Value::BIGINT(r.node_id));
        output.data[1].SetValue(i, Value::BIGINT(r.component_id));
        output.data[2].SetValue(i, Value::INTEGER(r.component_size));
    }
    state.offset += count;
}

// ─────────────────────────────────────────────────────────────────────────────
// label_groups(
//     ids      BIGINT[],         -- group IDs (one per group)
//     pixels   INTEGER[][],      -- sorted pixel_id arrays (one per group)
//     grid_w   INTEGER,          -- grid width for pixel_id → (x,y)
//     distance FLOAT    = 1.0,   -- max connecting distance
//     metric   VARCHAR  = 'L1',  -- 'L1' | 'L2' | 'Linf'
//     grid_ids BIGINT[] = NULL   -- optional: grid isolation for mass labeling
// ) → TABLE(group_id BIGINT, component_id BIGINT, component_size INTEGER)
//
// Mass labeling: pass groups from multiple grids with parallel grid_ids array.
// Groups with different (non-zero) grid_ids are never connected.
// ─────────────────────────────────────────────────────────────────────────────

struct LabelGroupsData : public TableFunctionData {
    std::vector<group_label::PixelGroup> groups;
    float                                distance;
    group_label::Metric                  metric;
};

static unique_ptr<FunctionData> label_groups_bind(
    ClientContext &,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names)
{
    auto data = make_uniq<LabelGroupsData>();

    // Parse ids
    std::vector<int64_t> ids;
    for (const auto &v : ListValue::GetChildren(input.inputs[0]))
        ids.push_back(v.IsNull() ? 0LL : v.GetValue<int64_t>());

    // Parse grid_ids (optional, param index 5)
    std::vector<int64_t> grid_ids(ids.size(), 0);
    if (input.inputs.size() > 5 && !input.inputs[5].IsNull()) {
        size_t k = 0;
        for (const auto &v : ListValue::GetChildren(input.inputs[5]))
            if (k < grid_ids.size()) grid_ids[k++] = v.IsNull() ? 0LL : v.GetValue<int64_t>();
    }

    int32_t grid_w = input.inputs[2].GetValue<int32_t>();

    data->distance = (input.inputs.size() > 3 && !input.inputs[3].IsNull())
        ? (float)input.inputs[3].GetValue<double>() : 1.0f;

    std::string metric_str = (input.inputs.size() > 4 && !input.inputs[4].IsNull())
        ? input.inputs[4].GetValue<std::string>() : "L1";
    data->metric = group_label::parse_metric(metric_str);

    // Build PixelGroup objects
    const auto &pix_list = ListValue::GetChildren(input.inputs[1]);
    data->groups.resize(ids.size());
    for (size_t i = 0; i < ids.size() && i < pix_list.size(); ++i) {
        auto &g = data->groups[i];
        g.id      = ids[i];
        g.grid_id = grid_ids[i];
        if (!pix_list[i].IsNull()) {
            std::vector<int32_t> pids;
            for (const auto &v : ListValue::GetChildren(pix_list[i]))
                if (!v.IsNull()) pids.push_back(v.GetValue<int32_t>());
            g.build(pids.data(), (int32_t)pids.size(), grid_w);
        }
    }

    return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::INTEGER};
    names = {"group_id", "component_id", "component_size"};
    return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> label_groups_init(
    ClientContext &, TableFunctionInitInput &input)
{
    const auto &d = input.bind_data->Cast<LabelGroupsData>();
    auto state = make_uniq<ResultState>();
    state->rows = group_label::label_groups(d.groups, d.distance, d.metric);
    return std::move(state);
}

static void label_groups_func(
    ClientContext &, TableFunctionInput &input, DataChunk &output)
{
    emit_results(output, input.global_state->Cast<ResultState>());
}

// ─────────────────────────────────────────────────────────────────────────────
// connected_components — all forms share CCData, cc_init, cc_func
// ─────────────────────────────────────────────────────────────────────────────

struct CCData : public TableFunctionData {
    std::vector<int64_t> node_ids, edge_a, edge_b;
};

static unique_ptr<GlobalTableFunctionState> cc_init(
    ClientContext &, TableFunctionInitInput &input)
{
    const auto &d = input.bind_data->Cast<CCData>();
    auto state = make_uniq<ResultState>();
    state->rows = group_label::connected_components(d.node_ids, d.edge_a, d.edge_b);
    return std::move(state);
}

static void cc_func(
    ClientContext &, TableFunctionInput &input, DataChunk &output)
{
    emit_results(output, input.global_state->Cast<ResultState>());
}

static const vector<LogicalType> CC_RETURN_TYPES = {
    LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::INTEGER};
static const vector<string> CC_NAMES = {"node_id", "component_id", "component_size"};

static void infer_nodes(CCData &data) {
    std::unordered_set<int64_t> seen;
    for (auto id : data.edge_a) seen.insert(id);
    for (auto id : data.edge_b) seen.insert(id);
    data.node_ids.assign(seen.begin(), seen.end());
}

static void list_val_to_vec(const Value &v, std::vector<int64_t> &out) {
    for (const auto &c : ListValue::GetChildren(v))
        if (!c.IsNull()) out.push_back(c.GetValue<int64_t>());
}

// ── Array forms ───────────────────────────────────────────────────────────────

// connected_components(node_ids[], edge_a[], edge_b[])
static unique_ptr<FunctionData> cc_bind3(
    ClientContext &,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names)
{
    auto data = make_uniq<CCData>();
    list_val_to_vec(input.inputs[0], data->node_ids);
    list_val_to_vec(input.inputs[1], data->edge_a);
    list_val_to_vec(input.inputs[2], data->edge_b);
    return_types = CC_RETURN_TYPES; names = CC_NAMES;
    return std::move(data);
}

// connected_components(edge_a[], edge_b[])  — nodes inferred from endpoints
static unique_ptr<FunctionData> cc_bind2(
    ClientContext &,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names)
{
    auto data = make_uniq<CCData>();
    list_val_to_vec(input.inputs[0], data->edge_a);
    list_val_to_vec(input.inputs[1], data->edge_b);
    infer_nodes(*data);
    return_types = CC_RETURN_TYPES; names = CC_NAMES;
    return std::move(data);
}

// ── $variable + table-name forms ──────────────────────────────────────────────
// Arguments beginning with '$' are resolved from ClientConfig::user_variables —
// the same session context, so temp tables and subquery results are fully
// accessible via SET VARIABLE before the call.
//
// $variable usage:
//   SET VARIABLE ea = (SELECT list(src) FROM any_table);  -- temp tables work!
//   SET VARIABLE eb = (SELECT list(dst) FROM any_table);
//   SELECT * FROM connected_components('$ea', '$eb');
//
// 2-arg ($ea, $eb):              nodes inferred from edge endpoints
// 3-arg ($node_ids, $ea, $eb):   explicit node list (captures isolated nodes)
//
// Table-name usage (persistent tables only — see comment in code):
//   SELECT * FROM connected_components('my_edges', 'src', 'dst');
//   SELECT * FROM connected_components('my_nodes', 'id', 'my_edges', 'src', 'dst');

static void resolve_var(ClientContext &context,
                        const std::string &arg,
                        std::vector<int64_t> &out)
{
    std::string var_name = arg.substr(1);   // strip leading $
    auto &vars = ClientConfig::GetConfig(context).user_variables;
    auto it = vars.find(var_name);
    if (it == vars.end())
        throw BinderException(
            "connected_components: variable '$%s' is not set. "
            "Use: SET VARIABLE %s = (SELECT list(...) FROM ...)",
            var_name, var_name);
    list_val_to_vec(it->second, out);
}

static std::string qi(const std::string &s) {
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}

static unique_ptr<FunctionData> cc_str_bind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names)
{
    auto data = make_uniq<CCData>();
    const idx_t n = input.inputs.size();
    const std::string first = input.inputs[0].GetValue<std::string>();
    const bool var_form = !first.empty() && first[0] == '$';

    if (var_form) {
        // ── $variable form ────────────────────────────────────────────────
        if (n == 2) {
            // ('$ea', '$eb') — nodes inferred
            resolve_var(context, first, data->edge_a);
            resolve_var(context, input.inputs[1].GetValue<std::string>(), data->edge_b);
            infer_nodes(*data);
        } else if (n == 3) {
            // ('$node_ids', '$ea', '$eb')
            resolve_var(context, first, data->node_ids);
            resolve_var(context, input.inputs[1].GetValue<std::string>(), data->edge_a);
            resolve_var(context, input.inputs[2].GetValue<std::string>(), data->edge_b);
        } else {
            throw BinderException(
                "connected_components: $variable form takes "
                "2 args ('$ea', '$eb') or 3 args ('$node_ids', '$ea', '$eb')");
        }
    } else {
        // ── Table-name form ───────────────────────────────────────────────
        // Needs a fresh Connection to avoid deadlocking on the active query's
        // ClientContext lock. Side effect: only PERSISTENT tables are visible —
        // temp tables are session-scoped. Use the $variable form for temp tables.
        Connection conn(DatabaseInstance::GetDatabase(context));

        auto fetch_e = [&](QueryResult &res) {
            for (auto chunk = res.Fetch(); chunk && chunk->size() > 0; chunk = res.Fetch())
                for (idx_t i = 0; i < chunk->size(); i++) {
                    data->edge_a.push_back(chunk->data[0].GetValue(i).GetValue<int64_t>());
                    data->edge_b.push_back(chunk->data[1].GetValue(i).GetValue<int64_t>());
                }
        };
        auto fetch_n = [&](QueryResult &res) {
            for (auto chunk = res.Fetch(); chunk && chunk->size() > 0; chunk = res.Fetch())
                for (idx_t i = 0; i < chunk->size(); i++)
                    data->node_ids.push_back(chunk->data[0].GetValue(i).GetValue<int64_t>());
        };

        std::string edge_tbl, src_col, dst_col;
        if (n == 5) {
            std::string node_tbl = input.inputs[0].GetValue<std::string>();
            std::string node_col = input.inputs[1].GetValue<std::string>();
            edge_tbl = input.inputs[2].GetValue<std::string>();
            src_col  = input.inputs[3].GetValue<std::string>();
            dst_col  = input.inputs[4].GetValue<std::string>();
            auto nr = conn.Query("SELECT DISTINCT " + qi(node_col) + "::BIGINT FROM " +
                                 qi(node_tbl) + " WHERE " + qi(node_col) + " IS NOT NULL");
            if (nr->HasError()) throw BinderException("connected_components: %s", nr->GetError());
            fetch_n(*nr);
        } else {
            edge_tbl = input.inputs[0].GetValue<std::string>();
            src_col  = input.inputs[1].GetValue<std::string>();
            dst_col  = input.inputs[2].GetValue<std::string>();
        }
        auto er = conn.Query("SELECT " + qi(src_col) + "::BIGINT, " + qi(dst_col) + "::BIGINT" +
                             " FROM " + qi(edge_tbl) +
                             " WHERE " + qi(src_col) + " IS NOT NULL"
                             " AND "   + qi(dst_col) + " IS NOT NULL");
        if (er->HasError()) throw BinderException("connected_components: %s", er->GetError());
        fetch_e(*er);
        if (n == 3) infer_nodes(*data);
    }

    return_types = CC_RETURN_TYPES; names = CC_NAMES;
    return std::move(data);
}

// ─────────────────────────────────────────────────────────────────────────────
// Load
// ─────────────────────────────────────────────────────────────────────────────

static void LoadInternal(ExtensionLoader &loader) {
    // label_groups: 4 overloads for optional trailing args.
    // Use DOUBLE not FLOAT: literal 1.0 is DECIMAL in DuckDB and casts to DOUBLE, not FLOAT.
    const LogicalType T_IDS  = LogicalType::LIST(LogicalType::BIGINT);
    const LogicalType T_PIX  = LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER));
    const LogicalType T_GIDS = LogicalType::LIST(LogicalType::BIGINT);

    // label_groups(ids, pixels, grid_w)
    loader.RegisterFunction(TableFunction("label_groups",
        {T_IDS, T_PIX, LogicalType::INTEGER},
        label_groups_func, label_groups_bind, label_groups_init));

    // label_groups(ids, pixels, grid_w, distance)
    loader.RegisterFunction(TableFunction("label_groups",
        {T_IDS, T_PIX, LogicalType::INTEGER, LogicalType::DOUBLE},
        label_groups_func, label_groups_bind, label_groups_init));

    // label_groups(ids, pixels, grid_w, distance, metric)
    loader.RegisterFunction(TableFunction("label_groups",
        {T_IDS, T_PIX, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::VARCHAR},
        label_groups_func, label_groups_bind, label_groups_init));

    // label_groups(ids, pixels, grid_w, distance, metric, grid_ids)
    loader.RegisterFunction(TableFunction("label_groups",
        {T_IDS, T_PIX, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::VARCHAR, T_GIDS},
        label_groups_func, label_groups_bind, label_groups_init));

    const auto T_BL = LogicalType::LIST(LogicalType::BIGINT);

    // Array forms
    loader.RegisterFunction(TableFunction("connected_components",
        {T_BL, T_BL, T_BL}, cc_func, cc_bind3, cc_init));  // (nodes, ea, eb)
    loader.RegisterFunction(TableFunction("connected_components",
        {T_BL, T_BL},        cc_func, cc_bind2, cc_init));  // (ea, eb) — nodes inferred

    // $variable and table-name forms (VARCHAR dispatcher)
    loader.RegisterFunction(TableFunction("connected_components",
        {LogicalType::VARCHAR, LogicalType::VARCHAR},
        cc_func, cc_str_bind, cc_init));  // ('$ea', '$eb')
    loader.RegisterFunction(TableFunction("connected_components",
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        cc_func, cc_str_bind, cc_init));  // ('$node_ids','$ea','$eb') or (tbl,src,dst)
    loader.RegisterFunction(TableFunction("connected_components",
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
         LogicalType::VARCHAR, LogicalType::VARCHAR},
        cc_func, cc_str_bind, cc_init));  // (node_tbl,node_col,edge_tbl,src,dst)
}

void GroupLabelExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }
std::string GroupLabelExtension::Name() { return "group_label"; }
std::string GroupLabelExtension::Version() const { return "v0.1.0"; }

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(group_label, loader) {
    duckdb::LoadInternal(loader);
}
}
