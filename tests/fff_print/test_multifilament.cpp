#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// Tool index = filament id - 1; brim and skirt follow the wall filament.
TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool); // sparse + solid + top/bottom
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 }); // filament 3
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 }); // filament 2
}

// The override must survive tool ordering: object 1's walls print on their filament's
// tool, object 0 stays on the first. If dropped, every wall prints on tool 0.
TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 }); // infill not overridden: stays on F1
}

// max_layer_height can be shorter than the extruder count (normalization sizes it to the
// filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per-nozzle and read past the end. Shortened directly here to isolate that read;
// the other per-extruder keys stay extruder-length so slicing reaches the code under test.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4" },
        { "printer_extruder_id",       "1,2" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "max_layer_height",          "0.3" }, // deliberately one entry short
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}

// Pin: with enable_filament_mapping off (default), a multi-tool printer emits the
// filament index as the T number, exactly as before the mapping feature existed.
TEST_CASE("Multi-tool printer with mapping disabled emits filament indices as tool numbers", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "2" },
        { "enable_prime_tower",             "0" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);   // filament 2 selected as T1
    REQUIRE(gcode.find("\nT2") == std::string::npos);   // never a tool beyond the filaments
}

// With enable_filament_mapping on and filament_map_mode Manual, a non-BBL multi-tool
// printer must honor the user's filament->extruder map, both at the Print level and in
// the flush-reorder path that groups by it.
TEST_CASE("Manual filament map is honored by the engine when mapping is enabled", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "2,1,2" },   // filament 1->tool 2, 2->tool 1, 3->tool 2
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
    });
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);
    const std::vector<int> maps = print.get_filament_maps();   // 1-based extruders
    REQUIRE(maps.size() == 3);
    REQUIRE(maps[0] == 2);
    REQUIRE(maps[2] == 2);
    REQUIRE(print.get_extruder_id(0) == 1);   // filament 1 (0-based) -> physical tool 1 (0-based)
    REQUIRE(print.get_extruder_id(2) == 1);
}

// The emitted gcode itself must address physical tools, not filament indices, once mapping
// is enabled: filament 3 is mapped to physical tool 1 (0-based) and should never appear as T2.
TEST_CASE("Mapped filaments emit physical tool numbers in gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },   // filament 3 -> physical tool 1 (0-based)
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "nozzle_temperature_initial_layer", "200,210,230" },
        // Give each filament its own nozzle-variant config column so the per-filament
        // temperatures above aren't collapsed onto a shared column by the (unrelated)
        // nozzle-variant grouping.
        { "filament_extruder_variant",      "\"Direct Drive Standard\";\"Direct Drive Standard\";\"Direct Drive Standard\"" },
        { "filament_self_index",            "1,2,3" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);   // filament 3 arrives as physical T1
    REQUIRE(gcode.find("\nT2") == std::string::npos);   // the filament index 2 never leaks out
    // Filament 3's first-layer temperature (230) targets physical tool 1, not filament index 2.
    REQUIRE(gcode.find("M104 S230 T1") != std::string::npos);
    REQUIRE(gcode.find("T2") == std::string::npos);
}

TEST_CASE("Change filament gcode placeholders carry physical tool ids when mapped", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "change_filament_gcode", ";CF next=[next_extruder] fil=[next_filament_id]" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(gcode.find(";CF next=1 fil=2") != std::string::npos);   // physical 1, filament index 2 (0-based)
}

TEST_CASE("GCodeProcessor attributes mapped tool changes to the correct filament", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
    });
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);
    // Derived inversion: tool 0 holds filament 1, tool 1 holds filament 3 (both 1-based).
    const std::vector<int>& ivm = print.config().tool_filament_map.values;
    REQUIRE(ivm.size() == 2);
    REQUIRE(ivm[0] == 1);
    REQUIRE(ivm[1] == 3);

    // Processor-level attribution: physical T1 in the gcode carries filament 3 (index 2), not
    // filament 2 (index 1) -- run the GCodeProcessor over the exported gcode and check the
    // per-filament volume it recorded.
    const std::string gcode_str = Slic3r::Test::gcode(print);
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode_str;
    }
    GCodeProcessor proc;
    proc.apply_config(print.config());
    proc.process_file(temp.string());
    const GCodeProcessorResult& result = proc.get_result();
    // Despite the "per_extruder" name, this map is keyed by filament id (see
    // GCodeProcessor::finalize assigning m_used_filaments.model_volumes_per_filament into it).
    const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;
    REQUIRE(volumes_per_filament.count(2) > 0);
    REQUIRE(volumes_per_filament.at(2) > 0.0);
    REQUIRE((volumes_per_filament.count(1) == 0 || volumes_per_filament.at(1) == 0.0));
}

TEST_CASE("GCodeProcessor attributes same-tool swapped filaments correctly", "[MultiFilament]") {
    bool enable_prime_tower = GENERATE(false, true);
    DYNAMIC_SECTION("enable_prime_tower " << enable_prime_tower) {
        DynamicPrintConfig config = multifilament_config(3, {
            { "nozzle_diameter",               "0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "enable_filament_mapping",        "1" },
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2; filament 1 alone on tool 1
            { "outer_wall_filament_id",         1 },
            { "inner_wall_filament_id",         1 },
            { "sparse_infill_filament_id",      2 },
            { "internal_solid_filament_id",     3 },
            { "top_surface_filament_id",        3 },
            { "bottom_surface_filament_id",     3 },
            { "enable_prime_tower",             enable_prime_tower ? "1" : "0" },
            { "filament_swap_gcode",            ";SWAP prev=[previous_filament_id] next=[next_filament_id] tool=[next_extruder]" },
        });
        Print print;
        Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);

        const std::string gcode_str = Slic3r::Test::gcode(print);
        ScopedTemporaryFile temp(".gcode");
        {
            std::ofstream os(temp.string());
            os << gcode_str;
        }
        GCodeProcessor proc;
        proc.apply_config(print.config());
        proc.process_file(temp.string());
        const GCodeProcessorResult& result = proc.get_result();
        const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;
        // All three filaments are used (1 alone on tool 1; 2 and 3 sharing tool 2 via a same-tool
        // swap) and must each get attributed nonzero extruded volume.
        REQUIRE(volumes_per_filament.count(0) > 0);
        REQUIRE(volumes_per_filament.at(0) > 0.0);
        REQUIRE(volumes_per_filament.count(1) > 0);
        REQUIRE(volumes_per_filament.at(1) > 0.0);
        REQUIRE(volumes_per_filament.count(2) > 0);
        REQUIRE(volumes_per_filament.at(2) > 0.0);
    }
}

TEST_CASE("Same-tool filaments emit the swap gcode instead of a tool change", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2
        { "outer_wall_filament_id",         2 },
        { "inner_wall_filament_id",         2 },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "top_surface_filament_id",        3 },
        { "bottom_surface_filament_id",     3 },
        { "enable_prime_tower",             "0" },
        { "filament_swap_gcode",            ";SWAP prev=[previous_filament_id] next=[next_filament_id] tool=[next_extruder]" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(gcode.find(";SWAP prev=1 next=2 tool=1") != std::string::npos); // 0-based filament ids, shared tool 1
    // The swap must not re-select the tool: no T1 line between the swap tag and the next extrusion.
    // Count T1 occurrences: exactly one (the initial selection of tool 1).
    size_t count = 0;
    for (size_t pos = gcode.find("\nT1"); pos != std::string::npos; pos = gcode.find("\nT1", pos + 1)) ++count;
    REQUIRE(count == 1);
}

// Type2 (non-BBL) wipe tower: for a same-tool filament pair (both filaments mapped to the
// same physical tool), the purge amount at the tower must be driven by flush_volumes_matrix,
// not collapsed to the flat prime_volume. Property-based: only the matrix entry for the
// swapped pair (filament index 1 -> 2, 0-based) changes across the three runs below; every
// other config value, including prime_volume, is identical. If purge tracked prime_volume
// instead of the matrix, all three runs would produce the same tower usage for filament
// index 2, regardless of the matrix entry.
TEST_CASE("Same-tool swap purge on a mapped Type2 tower scales with the flush matrix", "[MultiFilament]") {
    // Returns the total tower filament length attributed to filament index 2 (0-based, the
    // "3" in the shared-tool pair 2<->3) when the flush_volumes_matrix entry for that pair is
    // `flush_pair_value`.
    auto used_filament_for = [](float flush_pair_value) -> float {
        // 3x3 row-major matrix (filaments=3): diagonal 0, all off-diagonal 280 except the
        // swapped pair (1,2)/(2,1) which carries the value under test.
        std::string flush = "0,280,280,280,0," + std::to_string(flush_pair_value) + ",280," + std::to_string(flush_pair_value) + ",0";
        DynamicPrintConfig config = multifilament_config(3, {
            { "nozzle_diameter",               "0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "enable_filament_mapping",        "1" },
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2
            { "outer_wall_filament_id",         2 },
            { "inner_wall_filament_id",         2 },
            { "sparse_infill_filament_id",      3 },
            { "internal_solid_filament_id",     3 },
            { "top_surface_filament_id",        3 },
            { "bottom_surface_filament_id",     3 },
            { "enable_prime_tower",             "1" },
            { "purge_in_prime_tower",           "1" },
            { "flush_volumes_matrix",           flush },
            { "filament_swap_gcode",            ";SWAP prev=[previous_filament_id] next=[next_filament_id] tool=[next_extruder]" },
        });
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        // Print::apply() snapshots "filaments actually used" against the pre-update region
        // config to decide whether to force enable_prime_tower off (single-filament prints
        // don't need a tower). On a print's very first apply(), m_config hasn't been updated
        // yet at that snapshot point, so the check sees the default (single-filament) region
        // config and always disables the tower. Applying the same config again lets the check
        // run against the now-current region config, which correctly reports 2 filaments used.
        print.apply(model, config);
        print.process();
        const auto& used_filament = print.wipe_tower_data().used_filament;
        REQUIRE(used_filament.size() > 2);
        return used_filament[2];
    };

    const float low  = used_filament_for(0.f);
    const float mid  = used_filament_for(280.f);
    const float high = used_filament_for(3000.f);
    CAPTURE(low, mid, high);

    // Purge must track the matrix: strictly increasing, and by more than the noise floor
    // (mark_wiping_extrusions redirecting some purge to infill) could plausibly account for.
    REQUIRE(mid > low * 1.5);
    REQUIRE(high > mid * 1.5);
}

// R3.4 fix-round-1: filament_physical_map can claim two filaments are the same physical
// filament while filament_map puts them on DIFFERENT tools -- Print::validate() (Task 3) only
// rejects same-tool duplicate-tool conflicts, so this hostile/inconsistent combination is
// reachable. It must not corrupt real cross-tool behavior:
//  (a) ordering/gcode still treats this as a genuine tool change, not a same-tool swap -- that
//      decision is driven by actual tool equality, never by is_merged_pair alone.
//  (b) the Type2 tower still purges the transition at a real, matrix/prime-volume-driven level,
//      not zero. This is the regression this fix targets: Print.cpp's Type2 wipe_volumes
//      zeroing loop must skip a merge claim when the pair's tools differ, or it silently drops
//      an actual cross-tool purge to 0 (see Print.cpp's is_merged_pair(i,j) && get_extruder_id
//      match guard).
TEST_CASE("A physical-id merge claim across different tools does not suppress a real cross-tool purge", "[MultiFilament]") {
    // Returns the total tower filament length attributed to filament index 1 (0-based, the
    // "2" in the hostile cross-tool "merge" pair 1<->2) when the flush_volumes_matrix entry for
    // that pair is `flush_pair_value`. Mirrors "Same-tool swap purge on a mapped Type2 tower
    // scales with the flush matrix" above: if the zeroing loop wrongly clobbered this pair's
    // matrix cell to 0 regardless of `flush_pair_value`, every run below would report the same
    // (near-floor) tower usage; a real, unclobbered read tracks the configured value.
    //
    // single_extruder_multi_material=1 is what actually exercises the vulnerable read: the
    // Type2 tower's later purge lookup (~Print.cpp:4401) reads wipe_volumes[][] whenever
    // single_extruder_multi_material is set, regardless of same_tool_mapped_pair -- reachable
    // even though filament_mapping_enabled() (a different, higher-level check) requires !SEMM,
    // because get_extruder_id()/is_merged_pair() are lower-level and read filament_map/
    // filament_physical_map directly, unaffected by that requirement. That's exactly how a
    // hostile/leftover config (SEMM on, but filament_map still carrying distinct per-filament
    // values from an earlier non-SEMM setup) reaches this path.
    auto used_filament_for = [](float flush_pair_value) -> float {
        std::string flush = "0," + std::to_string(flush_pair_value) + "," + std::to_string(flush_pair_value) + ",0";
        DynamicPrintConfig config = multifilament_config(2, {
            { "nozzle_diameter",               "0.4,0.4" },
            { "single_extruder_multi_material", "1" },
            { "enable_filament_mapping",        "1" },
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2" },   // filament 1 -> tool 1, filament 2 -> tool 2 (different tools)
            { "filament_physical_map",          "5,5" },   // hostile: claims filaments 1 and 2 are the same physical filament
            { "outer_wall_filament_id",         1 },
            { "inner_wall_filament_id",         1 },
            { "sparse_infill_filament_id",      2 },
            { "internal_solid_filament_id",     2 },
            { "top_surface_filament_id",        2 },
            { "bottom_surface_filament_id",     2 },
            { "enable_prime_tower",             "1" },
            { "purge_in_prime_tower",           "1" },
            { "flush_volumes_matrix",           flush },
        });
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        // See "scales with flush matrix" above: a 2nd apply is needed for the true used-filament
        // count (and so enable_prime_tower isn't force-disabled).
        print.apply(model, config);
        print.process();

        REQUIRE(print.is_merged_pair(0, 1));                              // the hostile claim is in effect
        REQUIRE(print.get_extruder_id(0) != print.get_extruder_id(1));    // ...across different tools

        const auto& used_filament = print.wipe_tower_data().used_filament;
        REQUIRE(used_filament.size() > 1);
        return used_filament[1];
    };

    const float low  = used_filament_for(0.f);
    const float mid  = used_filament_for(280.f);
    const float high = used_filament_for(3000.f);
    CAPTURE(low, mid, high);

    // (b) the tower still purges the transition, tracking the matrix -- not clobbered to a
    // constant (near-zero-driven) value regardless of the configured flush amount.
    REQUIRE(mid > low * 1.5);
    REQUIRE(high > mid * 1.5);

    // (a) genuine tool change, not a same-tool swap: is_merged_pair alone must not gate that
    // decision (only actual tool equality does). Uses the `mid` config's own gcode.
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "1" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2" },
        { "filament_physical_map",          "5,5" },
        { "outer_wall_filament_id",         1 },
        { "inner_wall_filament_id",         1 },
        { "sparse_infill_filament_id",      2 },
        { "internal_solid_filament_id",     2 },
        { "top_surface_filament_id",        2 },
        { "bottom_surface_filament_id",     2 },
        { "enable_prime_tower",             "1" },
        { "purge_in_prime_tower",           "1" },
        { "flush_volumes_matrix",           "0,280,280,0" },
        { "filament_swap_gcode",            ";SWAP prev=[previous_filament_id] next=[next_filament_id] tool=[next_extruder]" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    print.apply(model, config);
    const std::string gcode_str = Slic3r::Test::gcode(print);
    // The raw filament_swap_gcode template also appears verbatim in the trailing config-echo
    // comment block, so look for the *expanded* swap tag (concrete filament ids), not a bare
    // ";SWAP" substring match.
    CHECK(gcode_str.find("SWAP prev=0 next=1") == std::string::npos);
    CHECK(gcode_str.find("SWAP prev=1 next=0") == std::string::npos);
    CHECK(gcode_str.find("\nT1") != std::string::npos);
}

// R3.4: filaments mapped to the same physical filament id merge and need no swap gcode,
// even though they still share a tool the same way a plain same-tool pair would.
TEST_CASE("Same-tool merged filaments need no swap gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2
        { "filament_physical_map",          "0,5,5" },   // filaments 2 and 3 merge to physical filament 5
        { "wall_filament",                  "2" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    REQUIRE(print.validate().string.empty());   // no filament_swap_gcode set, still clean: the pair merges
}

// Same shared tool, but the two physical ids differ: this is a genuine swap, not a merge, so
// the R2 swap-gcode requirement still applies.
TEST_CASE("Same-tool filaments with distinct physical ids still require swap gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2
        { "filament_physical_map",          "0,5,6" },   // filaments 2 and 3 map to DIFFERENT physical filaments
        { "wall_filament",                  "2" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    const StringObjectException err = print.validate();
    REQUIRE_FALSE(err.string.empty());
    REQUIRE(err.string.find("Filament swap G-code") != std::string::npos);
}

// R3.4: the mode-independent overflow guard must also treat a merged pair as one demand slot,
// not two -- an all-merged overflow needs no swap gcode either.
TEST_CASE("Overflow guard treats merged filaments as one demand slot", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },   // 2 tools
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        // default mode (fmmAutoForFlush) - exercises the mode-independent guard directly.
        { "filament_physical_map",          "0,5,5" },   // filaments 2 and 3 merge; filament 1 unassigned
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    // Demand = 2 (physical id 5's group, plus unassigned filament 1) == tool_count(2): no overflow.
    REQUIRE(print.validate().string.empty());
}

// is_merged_pair reads m_config.filament_physical_map directly: true only when both filament
// ids resolve to the same nonzero physical id; unassigned (0) and out-of-range ids never merge.
TEST_CASE("is_merged_pair reports merge status from the physical filament map", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },
        { "filament_physical_map",          "0,5,5" },   // filament 1 unassigned; filaments 2, 3 merge to id 5
        { "wall_filament",                  "2" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);

    CHECK(print.is_merged_pair(1, 2));         // filaments 2 and 3 (0-based 1, 2): both physical id 5
    CHECK_FALSE(print.is_merged_pair(0, 1));   // filament 1 (0-based 0) is unassigned (physical id 0)
    CHECK_FALSE(print.is_merged_pair(0, 2));
    CHECK_FALSE(print.is_merged_pair(1, 5));   // filament id 5 (0-based) is out of range
}

TEST_CASE("Shared-tool mappings require a filament swap gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,1" },   // used filaments 1 and 3 both on tool 1
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });

    SECTION("without filament_swap_gcode - should reject") {
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE_FALSE(err.string.empty());
        REQUIRE(err.string.find("Filament swap G-code") != std::string::npos);
    }

    SECTION("with filament_swap_gcode - should accept") {
        config.set_key_value("filament_swap_gcode", new ConfigOptionString(";SWAP prev=[previous_filament_id] next=[next_filament_id]"));
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE(err.string.empty());
    }
}

TEST_CASE("Validation accepts unused filaments sharing a tool", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,1,2" },   // filament 2 unused; collision harmless
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "3" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    REQUIRE(print.validate().string.empty());
}

// Tool inversion: tool_filament_map must record the FIRST filament active on each tool at
// print start, not the last one processed during the inversion loop. This test verifies the
// inversion is semantically correct: when multiple filaments share a tool and ascend by ID,
// tool_filament_map captures the first-assigned (first by ascending ID order), which is the
// correct initial filament for that tool. Re-import via GCodeProcessor::apply_config ensures
// the inversion roundtrips correctly.
TEST_CASE("Tool inversion captures first-assigned filament on shared tools", "[MultiFilament]") {
    // Filaments 1 and 2 share tool 1 (0-based); filament 1 prints FIRST (walls), filament 2 later (infill).
    // Last-writer-wins iteration (ascending ID: 1,2) would overwrite tool 1 with filament 2.
    // First-assignment-only logic correctly keeps filament 1 (the first assigned/first-by-ID).
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,1" },     // both filaments on same tool 1 (1-based)
        { "outer_wall_filament_id",         1 },         // filament 1 prints first (walls)
        { "inner_wall_filament_id",         1 },
        { "sparse_infill_filament_id",      2 },         // filament 2 prints later (infill, same-tool swap)
        { "internal_solid_filament_id",     2 },
        { "top_surface_filament_id",        2 },
        { "bottom_surface_filament_id",     2 },
        { "enable_prime_tower",             "0" },
    });
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);

    // Check that tool_filament_map correctly records the FIRST filament on the shared tool.
    // Tool 1 (index 0) should have filament 1 (1-based, first to print/first-by-ID), not filament 2.
    const std::vector<int>& tool_map = print.config().tool_filament_map.values;
    REQUIRE(tool_map.size() == 2);  // 2 tools (from nozzle_diameter "0.4,0.4")
    REQUIRE(tool_map[0] == 1);     // tool 1 has filament 1 (first assigned, not last)
    REQUIRE(tool_map[1] == 0);     // tool 2 is unused

    // Verify through GCodeProcessor that both filaments get correct usage attribution
    // even when they share a tool and tool_filament_map uses the first-filament inversion.
    const std::string gcode_str = Slic3r::Test::gcode(print);
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode_str;
    }
    GCodeProcessor proc;
    proc.apply_config(print.config());
    proc.process_file(temp.string());
    const GCodeProcessorResult& result = proc.get_result();
    const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;
    // Both filaments are used (sharing tool 1 via a same-tool swap)
    // and must each get attributed nonzero extruded volume.
    REQUIRE(volumes_per_filament.count(0) > 0);
    REQUIRE(volumes_per_filament.at(0) > 0.0);
    REQUIRE(volumes_per_filament.count(1) > 0);
    REQUIRE(volumes_per_filament.at(1) > 0.0);
}

// GCodeProcessor attribution with initial T-command having FILAMENT_CHANGE tag:
// Verify that tool activations are self-describing when FILAMENT_CHANGE tags are emitted
// on every T<physical> for mapped printers (not just same-tool swaps). This eliminates
// dependence on tool_filament_map inversion which can be wrong when multiple filaments
// share a tool and print order diverges from ascending-ID.
TEST_CASE("GCodeProcessor uses FILAMENT_CHANGE tags on every mapped tool activation", "[MultiFilament]") {
    // Setup: two filaments (1,2) share tool 0; filament 2 is higher ID but prints first
    // (e.g., custom feature order or flush-minimizing reorder). Without tags on initial
    // T-command, processor would invert using tool_filament_map[0]==1 and misattribute
    // filament 2's extrusions to filament 1.

    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,1" },     // both filaments on tool 1 (0-based tool 0)
        { "wall_filament",                  "2" },       // filament 2 (higher ID) assigned to walls
        { "inner_wall_filament_id",         "2" },
        { "sparse_infill_filament_id",      "2" },       // same filament throughout; no swaps
        { "internal_solid_filament_id",     "2" },
        { "top_surface_filament_id",        "2" },
        { "bottom_surface_filament_id",     "2" },
        { "enable_prime_tower",             "0" },
    });

    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);

    // tool_filament_map[0]==1 (ascending-ID choice: filament 1, lower ID)
    // but filament 2 actually prints
    const std::vector<int>& tool_map = print.config().tool_filament_map.values;
    REQUIRE(tool_map[0] == 1);

    // Verify that with FILAMENT_CHANGE tag on the T-command, processor attributes ALL
    // extrusions to filament 2 (0-based index 1), not to filament 1.
    const std::string gcode_str = Slic3r::Test::gcode(print);
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode_str;
    }
    GCodeProcessor proc;
    proc.apply_config(print.config());
    proc.process_file(temp.string());
    const GCodeProcessorResult& result = proc.get_result();
    const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;

    // Only filament 2 (0-based index 1) is used in this config (everything assigned to it).
    // Verify that filament 1 (0-based index 0) gets zero (or negligible) volume,
    // and filament 2 gets nonzero volume.
    // Without the FILAMENT_CHANGE tag on T-command, processor would attribute all
    // volume to filament 1 (the wrong one from tool_filament_map).
    REQUIRE(volumes_per_filament.count(1) > 0);
    REQUIRE(volumes_per_filament.at(1) > 0.0);
    // Filament 1 should have zero volume (it was never selected to print)
    if (volumes_per_filament.count(0) > 0) {
        REQUIRE(volumes_per_filament.at(0) < 0.01);  // negligible; allows for rounding
    }
}

// Regression: the per-map "same tool" / "incomplete map" / "tool this printer does not have"
// checks used to run whenever filament_mapping_enabled(), with no check on filament_map_mode.
// filament_map_mode defaults to fmmAutoForFlush (not fmmManual), and filament_map defaults to
// all-1s -- so merely painting a second filament onto a plate (2 used filaments, both landing
// on the default map's tool 1) tripped "Filaments 1 and 2 are mapped to the same tool" and
// disabled the slice button, even though the mapping dialog (the only way to set a real map)
// only opens once the user presses Slice. That's a deadlock: the dialog needed to fix the map
// was unreachable because the button that opens it was disabled. The strict checks now only
// apply once filament_map_mode is actually fmmManual (set by the dialog, or loaded from a
// stored 3MF plate); until then, ToolOrdering's non-manual path resolves grouping instead of
// reading this map.
TEST_CASE("Default filament_map_mode does not trip duplicate-tool validation", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        // filament_map_mode intentionally left at its default (fmmAutoForFlush, not fmmManual).
        { "filament_map",                   "1,1" },   // default identity map: both on tool 1
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    REQUIRE(print.validate().string.empty());
}

// The one case that IS genuinely unsliceable regardless of mapping mode: more used filaments
// than the printer has tools (a tool holds one filament per plate). This must still disable
// the slice button even before a manual map exists.
TEST_CASE("Overflow with multiple filaments on same tool requires swap gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },   // 2 tools
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,1" },   // 3 filaments on 2 tools (filaments 1 and 3 on tool 1)
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });

    SECTION("without filament_swap_gcode - should reject") {
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE_FALSE(err.string.empty());
        REQUIRE(err.string.find("Filament swap G-code") != std::string::npos);
    }

    SECTION("with filament_swap_gcode - should accept") {
        config.set_key_value("filament_swap_gcode", new ConfigOptionString(";SWAP prev=[previous_filament_id] next=[next_filament_id]"));
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE(err.string.empty());
    }
}

TEST_CASE("Overflow in auto-flush mode requires swap gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },   // 2 tools
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        // filament_map_mode intentionally left at default (fmmAutoForFlush, not fmmManual)
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });

    SECTION("without filament_swap_gcode - should reject overflow") {
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE_FALSE(err.string.empty());
        REQUIRE(err.string.find("Filament swap G-code") != std::string::npos);
    }

    SECTION("with filament_swap_gcode - should accept overflow") {
        config.set_key_value("filament_swap_gcode", new ConfigOptionString(";SWAP prev=[previous_filament_id] next=[next_filament_id]"));
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE(err.string.empty());
    }
}

// Regression: Print::validate's mode-independent overflow guard must accept a 4-tool/5-filament
// shape (one more filament than tools, so two filaments must share a tool) once filament_swap_
// gcode is set, and reject it otherwise -- this is what made the U1/WonderMaker ZR Ultra bases
// unreachable while they shipped enable_filament_mapping=1 with an empty filament_swap_gcode.
// This test pins ONLY Print::validate's behavior on a synthetic config built in-memory; it does
// NOT read or exercise the shipped profile JSON (see "Filament-mapping printer profiles ship a
// non-empty filament_swap_gcode" in tests/libslic3r/test_config.cpp for that, the actual
// profile-file regression coverage).
TEST_CASE("Print::validate accepts a 4-tool/5-filament overflow once swap gcode is set", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(5, {
        { "nozzle_diameter",               "0.4,0.4,0.4,0.4" },   // 4 tools
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,3,4,1" },   // 5 filaments on 4 tools (1 and 5 share tool 1)
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "5" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });

    SECTION("without filament_swap_gcode - should reject") {
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE_FALSE(err.string.empty());
        REQUIRE(err.string.find("Filament swap G-code") != std::string::npos);
    }

    SECTION("with filament_swap_gcode - should accept") {
        config.set_key_value("filament_swap_gcode", new ConfigOptionString(";SWAP prev=[previous_filament_id] next=[next_filament_id]"));
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE(err.string.empty());
    }
}

// Regression: the toolchange gate in GCodeWriter::toolchange used to key off
// `enable_filament_mapping && !m_is_bbl_printers` alone, weaker than every other mapping
// site's `multiple_extruders && !single_extruder_multi_material` check. A single-nozzle SEMM
// profile with a stale/leftover enable_filament_mapping=1 flag then entered the physical-tool
// path and emitted m_curr_extruder_id (always 0 on a single nozzle) for every tool change,
// collapsing all filament changes to T0. single_extruder_multi_material defaults to true and
// multifilament_config() defaults to a single "0.4" nozzle_diameter, so this is a genuine SEMM
// single-nozzle configuration.
// Regression: reorder_filaments_for_minimum_flush_volume (ToolOrderUtils.cpp) used to hard-code
// exactly 2 groups (matching BBL's only supported case). Since manual filament maps started
// carrying real tool indices for opted-in non-BBL multi-tool printers, any filament mapped to
// tool 3+ (0-based group index >= 2) joined no group, was never placed into a layer's filament
// sequence, and silently vanished from the gcode entirely.
TEST_CASE("Filaments mapped to tool 3+ are not dropped from gcode", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(4, {
        { "nozzle_diameter",               "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,3,4" },   // identity: filament N -> tool N
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "4" },
        { "solid_infill_filament",          "4" },
        { "enable_prime_tower",             "0" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    // Attribute by role (not just raw "T3" substring search - the writer can emit an unrelated T3
    // via priming/homing, which would make a plain substring check pass even with filament 4's
    // extrusions genuinely dropped).
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0 });   // filament 1 -> tool 1
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 3 });   // filament 4 -> tool 4; used to vanish
}

// Same regression, exercising the merge/round-robin path across three simultaneously-used
// groups (walls, sparse infill, and solid infill all print on the same layers), covering the
// >2-group case in the final per-layer sequence assembly, not just the initial grouping. Sparse
// and solid infill share the same "infill" comment tag regardless of filament, so assigning them
// different filaments/tools and asserting the union covers both is what would catch either one
// vanishing.
TEST_CASE("Filaments mapped to tools 3 and 4 survive the multi-group flush reorder merge", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,3,4" },   // filament 1->tool 1, 2->tool 3, 3->tool 4
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0 });         // filament 1 -> tool 1
    CHECK(tools_for_role(gcode, "infill")    == (std::set<int>{ 2, 3 }));   // filaments 2 and 3 -> tools 3 and 4; either used to vanish
}

// R3.4: merged filaments (2 and 4, same physical id, both on tool 2 along with distinct
// filament 3) must print contiguously within every layer -- no non-member filament may appear
// between the merged group's first and last member. toolchange_ordering=cyclic forces the
// per-layer custom sequence to plain ascending filament id (2, 3, 4 for tool 2's used set),
// which sandwiches distinct filament 3 between the merged pair -- exactly the interleave the
// mandatory contiguity post-pass must undo, since zero flush alone can't reach the custom-
// sequence branch at all.
//
// Per-layer activation order is read from the actual gcode, not internal ToolOrdering state:
// every cross-tool activation (via change_filament_gcode) and same-tool swap (via
// filament_swap_gcode) is tagged with the same "TC fil=<0-based filament id>" marker, and
// ";LAYER_CHANGE" delimits layers.
TEST_CASE("Merged filaments print contiguously per layer even under cyclic ascending ordering", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(4, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2,2" },   // filament 1 alone on tool 1; 2, 3, 4 share tool 2
        { "filament_physical_map",          "0,5,0,5" },   // filaments 2 and 4 merge to physical id 5; filament 3 stays distinct
        { "toolchange_ordering",            "cyclic" },    // forces plain ascending-id per-layer sequencing
        { "outer_wall_filament_id",         2 },           // merged-group member; walls print on every layer
        { "inner_wall_filament_id",         3 },           // distinct; walls print on every layer
        { "sparse_infill_filament_id",      4 },           // merged-group member; present on most layers
        { "internal_solid_filament_id",     1 },           // tool 1, unrelated to the merged group
        { "top_surface_filament_id",        1 },
        { "bottom_surface_filament_id",     1 },
        { "enable_prime_tower",             "0" },
        { "change_filament_gcode",          ";TC fil=[next_filament_id]" },
        { "filament_swap_gcode",            ";TC fil=[next_filament_id]" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);

    std::vector<std::vector<int>> per_layer_sequence(1);
    {
        std::istringstream iss(gcode);
        std::string line;
        const std::string tc_prefix = ";TC fil=";
        while (std::getline(iss, line)) {
            if (line == ";LAYER_CHANGE") {
                per_layer_sequence.emplace_back();
                continue;
            }
            if (line.rfind(tc_prefix, 0) == 0)
                per_layer_sequence.back().push_back(std::stoi(line.substr(tc_prefix.size())));
        }
    }

    const std::set<int> merged_group{ 1, 3 }; // 0-based filament ids for filaments 2 and 4
    bool saw_all_three_together = false;
    for (const auto& seq : per_layer_sequence) {
        int first_member = -1, last_member = -1;
        for (size_t i = 0; i < seq.size(); ++i) {
            if (merged_group.count(seq[i])) {
                if (first_member == -1)
                    first_member = (int) i;
                last_member = (int) i;
            }
        }
        if (first_member == -1)
            continue;
        if (std::find(seq.begin(), seq.end(), 2) != seq.end()) // 0-based filament 3
            saw_all_three_together = true;
        for (int i = first_member; i <= last_member; ++i)
            CHECK(merged_group.count(seq[i]) > 0); // no non-member between the group's first and last member
    }
    REQUIRE(saw_all_three_together); // sanity: the scene genuinely interleaves the trio somewhere
}

// R3.5: filaments merged to the same physical filament id (and tool) get a null transition --
// zero g-code between them, not just zero flush. Config: filament 1 alone on tool 1; filaments
// 2 and 3 share tool 2 and merge to physical id 5. Walls print with filament 2 on every layer,
// sparse infill with filament 3 (its merged partner) on every layer, so every layer's gcode
// contains a real cross-tool activation (tool 1 -> tool 2, landing on filament 2) followed by
// the merged-pair transition (filament 2 -> filament 3) this task targets.
static DynamicPrintConfig merged_pair_config(bool enable_prime_tower) {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,2" },   // filaments 2 and 3 share tool 2
        { "filament_physical_map",          "0,5,5" },   // filaments 2 and 3 merge to physical id 5
        { "outer_wall_filament_id",         2 },
        { "inner_wall_filament_id",         2 },
        { "sparse_infill_filament_id",      3 },
        { "internal_solid_filament_id",     3 },
        { "top_surface_filament_id",        3 },
        { "bottom_surface_filament_id",     3 },
        { "enable_prime_tower",             enable_prime_tower ? "1" : "0" },
        { "purge_in_prime_tower",           "1" },
        { "change_filament_gcode",          ";CHANGE_F[next_filament_id]\n" },
        { "filament_swap_gcode",            ";SWAP_F[previous_filament_id]_[next_filament_id]\n" },
        { "layer_height",                   "0.3" },      // cube_with_hole is 10mm tall -> ~33
                                                          // layers, so a suppressed per-layer
                                                          // marker would otherwise repeat many times
    });
    // Distinctive per-filament markers (0-based array, one entry per filament).
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({ ";START_F1\n", ";START_F2\n", ";START_F3\n" }));
    config.set_key_value("filament_end_gcode",   new ConfigOptionStrings({ ";END_F1\n",   ";END_F2\n",   ";END_F3\n" }));
    return config;
}

// Count non-overlapping occurrences of `needle` in `haystack`.
static size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// The exported gcode ends with a verbatim config-value dump (see "; CONFIG_BLOCK_START" /
// "; CONFIG_BLOCK_END" in GCode.cpp), which echoes every custom-gcode option's *raw* value --
// including our literal markers below, since they carry no placeholders to expand. Strip it so
// marker counts reflect only what was actually emitted as instructions.
static std::string strip_config_block(const std::string& gcode) {
    size_t start = gcode.find("; CONFIG_BLOCK_START");
    return start == std::string::npos ? gcode : gcode.substr(0, start);
}

TEST_CASE("Merged filaments emit no swap gcode or per-filament start/end gcode at their shared transition", "[MultiFilament]") {
    const std::string gcode = strip_config_block(Slic3r::Test::slice({ TestMesh::cube_with_hole }, merged_pair_config(false)));

    // No filament_swap_gcode marker anywhere: the 2->3 (and 3->2) transitions are null, not
    // same-tool swaps (same_tool_swap is exactly the case Task 5 upgrades to a null transition
    // once the pair is merged -- see GCodeWriter::toolchange).
    CHECK(gcode.find(";SWAP_F") == std::string::npos);
    // Filament 1 is unused in this scene, so tool 2 (filaments 2 and 3) is the only tool ever
    // addressed: at most one genuine cross-tool activation happens for the whole print (the
    // initial pick); every later 2<->3 transition within tool 2 must be the null transition under
    // test, not a repeat of change_filament_gcode.
    CHECK(count_occurrences(gcode, ";CHANGE_F") <= 1);
    // Likewise, filament_start_gcode/filament_end_gcode (unconditional on any real transition,
    // merged or not, pre-Task-5) must not repeat once per layer for the merged pair: across the
    // whole print, at most one real (non-merged) transition ever touches either marker.
    CHECK(count_occurrences(gcode, ";START_F2") + count_occurrences(gcode, ";START_F3") <= 1);
    // End gcode additionally fires once more at the final unload past the last layer (unrelated
    // to the merged transition under test), so allow one extra occurrence beyond the single real
    // transition -- the property under test is "doesn't repeat once per layer" (~33 layers), not
    // "fires exactly once ever".
    CHECK(count_occurrences(gcode, ";END_F2") + count_occurrences(gcode, ";END_F3") <= 2);

    // Both merged members still extrude their assigned features (role-attributed).
    CHECK_FALSE(tools_for_role(gcode, "infill").empty());    // filament 3 (tool 1, 0-based)
    CHECK_FALSE(tools_for_role(gcode, "perimeter").empty()); // filament 2 (tool 1, 0-based)
}

TEST_CASE("Merged filaments produce no wipe tower block for their shared transition and no runtime error", "[MultiFilament]") {
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, merged_pair_config(true));
    // The GCode.cpp:1955 "Wipe tower generation failed" throw (positional tcr-cursor mismatch)
    // is the failure mode if is_empty_wipe_tower_gcode and the tower planner's merged-transition
    // skip ever disagree; init_and_process_print would propagate any such exception.
    const std::string gcode = Slic3r::Test::gcode(print);
    CHECK_FALSE(tools_for_role(gcode, "infill").empty());
    CHECK_FALSE(tools_for_role(gcode, "perimeter").empty());

    // Compare tower toolchange count against an otherwise-identical unmerged config: the merged
    // pair's per-layer internal transition must never reach the tower planner, so it must produce
    // strictly fewer wipe tower toolchanges than the same scene with filament_physical_map cleared.
    DynamicPrintConfig unmerged_config = merged_pair_config(true);
    unmerged_config.set_key_value("filament_physical_map", new ConfigOptionInts({ 0, 0, 0 }));
    Print unmerged_print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, unmerged_print, unmerged_config);

    REQUIRE(print.wipe_tower_data().number_of_toolchanges < unmerged_print.wipe_tower_data().number_of_toolchanges);
}

// With single_extruder_multi_material=1, filament_mapping_enabled() is false, so
// merged_transition() reports "not merged" on the gcode side and every consumer there plans for
// a real transition. The tower planner's merged-transition skips in Print::_make_wipe_tower must
// be gated on the same predicate: if they still honor a stale filament_physical_map and skip
// plan_toolchange, the gcode side goes looking for a tcr slot that was never reserved and
// WipeTowerIntegration throws ("append_tcr was asked to do a toolchange it didn't expect").
TEST_CASE("A merged physical map is inert on a SEMM printer and slicing does not throw", "[MultiFilament]") {
    DynamicPrintConfig config = merged_pair_config(true);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    Print print;
    std::string gcode;
    REQUIRE_NOTHROW([&] {
        Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);
        gcode = strip_config_block(Slic3r::Test::gcode(print));
    }());
    // With mapping disabled the pair must NOT get null transitions: the per-layer 2<->3 changes
    // stay real filament changes, each emitting change_filament_gcode (contrast the <= 1 bound
    // the mapped, non-SEMM test above asserts for the same scene).
    CHECK(count_occurrences(gcode, ";CHANGE_F") > 1);
}

// Processor attribution: the merged transition still emits the FILAMENT_CHANGE tag (self-describing
// invariant), so both merged members must still get correct per-filament extruded volume even
// though there is no T-line or swap gcode between them.
TEST_CASE("GCodeProcessor attributes merged filaments correctly despite the null transition", "[MultiFilament]") {
    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, merged_pair_config(false));
    const std::string gcode_str = Slic3r::Test::gcode(print);
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode_str;
    }
    GCodeProcessor proc;
    proc.apply_config(print.config());
    proc.process_file(temp.string());
    const GCodeProcessorResult& result = proc.get_result();
    const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;
    REQUIRE(volumes_per_filament.count(1) > 0); // filament 2 (0-based 1): walls
    REQUIRE(volumes_per_filament.at(1) > 0.0);
    REQUIRE(volumes_per_filament.count(2) > 0); // filament 3 (0-based 2): infill
    REQUIRE(volumes_per_filament.at(2) > 0.0);
}

// Fix round 1: the Type1 (BBL-style) wipe tower planning loop's merged-transition skip must also
// update the per-nozzle "last filament" bookkeeping (NozzleStatusRecorder / nozzle_cur_filament_ids)
// to the NEW merged member, not just advance current_filament_id -- otherwise a later REAL
// transition's flush lookup reads the stale pre-merge predecessor's row. Task 4 only zeroed the
// flush_volumes_matrix cell for the merged pair itself ([i][j]/[j][i]); it never equalized the
// merged partners' full outbound rows, and flush_volumes_matrix is free-form user data, so those
// rows can genuinely diverge -- exactly what this test constructs.
//
// Property-based: filament 1 alone on tool 1 (unused by geometry directly, but present via
// internal_solid/top/bottom so tool 1 still gets picked at some layers); filaments 2 and 3 share
// tool 2 and merge to physical id 5; filament 4 also shares tool 2 and stays distinct.
// toolchange_ordering=cyclic forces the ascending sequence 2, 3, 4 on tool 2, so every layer that
// uses all three produces: real activation into filament 2, a null (merged) transition to
// filament 3, then a REAL transition to filament 4. Only the two matrix cells feeding that last
// transition (0-based 1->3, i.e. filament2->4, and 0-based 2->3, i.e. filament3->4) diverge between
// the two runs below. If the post-merge bookkeeping is correct, filament 4's tower usage tracks
// the row FROM filament 3 (the actual occupant after the merge); if bookkeeping stayed stale at
// the merge's first member, it would instead track the row from filament 2 -- swapping which row
// is large would then swap which run purges more, in the wrong direction.
TEST_CASE("Type1 tower purge after a merged pair uses the post-merge member's flush row", "[MultiFilament]") {
    // Reuses the exact scene proven (by the "Merged filaments print contiguously..." test above)
    // to interleave a merged pair with a distinct same-tool filament: outer_wall = merged member 1,
    // inner_wall = the distinct filament, sparse_infill = merged member 2, filaments 2 and 4 merge
    // to physical id 5, filament 3 stays distinct. toolchange_ordering=cyclic forces the natural
    // per-layer sequence 2, 3, 4 (ascending), which the contiguity post-pass (Task 4) then reorders
    // to 2, 4, 3 so the merged pair (2, 4) is adjacent -- i.e. every layer's tool 2 sequence is
    // filament2 (real) -> filament4 (merged, silent) -> filament3 (real).
    //
    // Type1's WipeTower::plan_toolchange stores the requested purge amount verbatim on the planned
    // ToolChangeResult (the `purge_volume` field) -- unlike the physical tower depth/length, which
    // (for a same-tool transition) is driven by the flat filament_prime_volume config, not the flush
    // matrix. Reading `purge_volume` off the specific planned transition is therefore the precise,
    // directly-targeted way to observe which matrix row a same-tool transition actually read,
    // without going through any depth/length indirection that isn't sensitive to it.
    auto purge_for_4_to_3 = [](float row_filament2_to_3, float row_filament4_to_3) -> float {
        // get_flush_volumes_matrix() (used by the Type1 planning loop, unlike Type2 which reads
        // flush_volumes_matrix directly) slices it into one filaments x filaments sub-matrix PER
        // NOZZLE: total size must be nozzle_nums * filaments^2. With 2 nozzles and 4 filaments,
        // that is 2 blocks of 16 cells. Put the same divergent cells (0-based: 1->2 and 3->2) in
        // both blocks so the result doesn't depend on which nozzle index the code resolves to.
        auto build_block = [](float cell_1_2, float cell_3_2) {
            std::vector<std::string> cells(16, "280");
            for (int i = 0; i < 4; ++i)
                cells[i * 4 + i] = "0";
            cells[1 * 4 + 2] = std::to_string(cell_1_2);
            cells[3 * 4 + 2] = std::to_string(cell_3_2);
            return cells;
        };
        std::vector<std::string> cells = build_block(row_filament2_to_3, row_filament4_to_3);
        std::vector<std::string> block1 = build_block(row_filament2_to_3, row_filament4_to_3);
        cells.insert(cells.end(), block1.begin(), block1.end());
        std::string matrix;
        for (size_t i = 0; i < cells.size(); ++i)
            matrix += (i ? "," : "") + cells[i];

        DynamicPrintConfig config = multifilament_config(4, {
            { "nozzle_diameter",               "0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "enable_filament_mapping",        "1" },
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2,2,2" },  // filament 1 alone on tool 1; 2, 3, 4 share tool 2
            { "filament_physical_map",          "0,5,0,5" },  // filaments 2 and 4 merge to physical id 5
            { "toolchange_ordering",            "cyclic" },   // forces plain ascending-id sequencing: 2, 3, 4
            { "wipe_tower_type",                "type1" },    // exercise the Type1 (BBL-style) planning loop
            // cube_with_hole leaves no genuine sparse-infill region (confirmed by a debug run: a
            // sparse_infill_filament_id assignment never appears in any layer's tool_changes at
            // all on this mesh), so put both merged members on region types verified to have real
            // area instead: walls (every layer) and solid/top/bottom infill (this small, mostly-
            // solid shape).
            { "outer_wall_filament_id",         2 },
            { "inner_wall_filament_id",         3 },
            { "internal_solid_filament_id",     4 },
            { "top_surface_filament_id",        4 },
            { "bottom_surface_filament_id",     4 },
            { "enable_prime_tower",             "1" },
            { "purge_in_prime_tower",           "1" },
            { "flush_volumes_matrix",           matrix },
            // append_full_config's config-dump validator requires flush_volumes_matrix sized
            // filaments^2 * flush_multiplier.size() (independent of get_flush_volumes_matrix's own
            // nozzle_diameter-keyed slicing in the Type1 loop); default flush_multiplier is a single
            // value, so widen it to one entry per nozzle (neutral 1.0, so it doesn't distort the
            // matrix values under test) to match the two-nozzle-sized matrix built above.
            { "flush_multiplier",               "1,1" },
        });
        Print print;
        Model model;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        // Print::apply() snapshots "filaments actually used" against the pre-update region config on
        // a print's very first apply() (see the Type2 purge test above for the same gotcha);
        // applying again lets the multi-filament check run against the current region config.
        print.apply(model, config);
        print.process();

        // Find the planned same-tool transition from filament 4 (0-based 3, the post-merge
        // occupant) to filament 3 (0-based 2, the distinct filament) and return its purge_volume.
        for (const auto& layer_changes : print.wipe_tower_data().tool_changes)
            for (const auto& tc : layer_changes)
                if (tc.initial_tool == 3 && tc.new_tool == 2)
                    return tc.purge_volume;
        FAIL("no filament4->filament3 transition found in the planned tool changes");
        return -1.f;
    };

    const float purge_when_row4_large = purge_for_4_to_3(1.f, 10000.f);
    const float purge_when_row4_small = purge_for_4_to_3(10000.f, 1.f);
    CAPTURE(purge_when_row4_large, purge_when_row4_small);

    // If bookkeeping is correct (updated to filament 4 during the merged step), this transition
    // reads row 4->3 (0-based 3->2): large (10000) in the first run, small (1) in the second. If
    // bookkeeping stayed stale at the merge's first member (filament 2), it would instead read row
    // 2->3 (0-based 1->2), which is set to the OPPOSITE value in each run (small/1 in the first,
    // large/10000 in the second) -- so stale bookkeeping doesn't just fail to show a difference,
    // it inverts the direction of this comparison.
    REQUIRE(purge_when_row4_large > purge_when_row4_small * 10.f);
}

TEST_CASE("Stale enable_filament_mapping flag does not collapse SEMM tool changes to T0", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "enable_filament_mapping",   "1" },   // stale flag; should be inert without multiple_extruders
        { "filament_map_mode",         "Manual" },
        { "filament_map",              "1,1,1" },
        { "wall_filament",             "1" },
        { "sparse_infill_filament",    "2" },
        { "solid_infill_filament",     "2" },
        { "enable_prime_tower",        "0" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);   // filament 2 still addressed by its own index
    REQUIRE(gcode.find("\nT2") == std::string::npos);   // never a tool beyond the filaments
}

// Regression: the Snapmaker U1 default profile's change_filament_gcode ends with a literal
// "T" + next_extruder -- the custom gcode supplies its own T-command. custom_gcode_changes_tool()
// detects that T and, at both suppression sites (GCode::set_extruder and WipeTowerIntegration's
// append_tcr), discards the writer's generated toolchange_command outright. On a mapped printer
// that command is the only carrier of the FILAMENT_CHANGE tag, so every cross-tool activation
// silently lost its tag -- degrading processor attribution to the tool_filament_map inversion
// fallback, which is known to misattribute when filaments share a tool print out of ID order.
TEST_CASE("FILAMENT_CHANGE tag survives a change_filament_gcode that supplies its own T", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",               "0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,3" },   // identity: filament N -> tool N, all distinct
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "0" },
        // Models the U1 profile: the custom gcode does its own tool selection.
        { "change_filament_gcode",          "M104 S0\nT[next_extruder]" },
    });

    Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::cube_with_hole }, print, config);
    const std::string gcode_str = Slic3r::Test::gcode(print);

    // Every physical T-command (cross-tool activation) must be followed by a FILAMENT_CHANGE tag
    // before the next extrusion move -- other bookkeeping lines (e.g. _FORCE_RESUME_FAN_SPEED) may
    // legitimately sit in between, so this doesn't require the tag on the very next line.
    GCodeReader reader;
    int  t_count      = 0;
    int  tag_count    = 0;
    bool awaiting_tag = false;
    reader.parse_buffer(gcode_str, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string raw(line.raw());
        if (awaiting_tag) {
            if (raw.find("FILAMENT_CHANGE") != std::string::npos) {
                ++tag_count;
                awaiting_tag = false;
            } else if (line.extruding(self)) {
                awaiting_tag = false; // reached real extrusion without a tag -- a miss
            }
        }
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1])) {
            ++t_count;
            awaiting_tag = true;
        }
    });
    REQUIRE(t_count >= 2);          // at least two cross-tool activations occurred
    REQUIRE(tag_count == t_count);  // every activation kept its tag

    // Processor-level attribution must follow the tags: each of the three filaments (one per
    // tool) gets nonzero extruded volume.
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode_str;
    }
    GCodeProcessor proc;
    proc.apply_config(print.config());
    proc.process_file(temp.string());
    const GCodeProcessorResult& result = proc.get_result();
    const auto& volumes_per_filament = result.print_statistics.model_volumes_per_extruder;
    for (int fi = 0; fi < 3; ++fi) {
        REQUIRE(volumes_per_filament.count(fi) > 0);
        REQUIRE(volumes_per_filament.at(fi) > 0.0);
    }
}
// Regression: on a mapped multi-tool printer (Snapmaker U1-shaped: 4 tools, filament mapping
// on, enable_prime_tower + independent_support_layer_height both on as the U1 process profile
// ships them), completing a slice and then re-applying the SAME unchanged config a second time
// - exactly what the GUI's post-slice background_process update does on every completed slice,
// rebuilding a fresh config from the current presets (not reusing the print's own resolved
// state) - used to flip enable_prime_tower/independent_support_layer_height and report
// APPLY_STATUS_CHANGED instead of UNCHANGED. This is what invalidates and visibly resets an
// otherwise-successful slice in the GUI.
//
// Root cause (two compounding staleness bugs in Print::apply(), PrintApply.cpp):
// 1. The EARLY normalize_fdm_2() call decides whether to force enable_prime_tower /
//    independent_support_layer_height off using `this->extruders(true)`, read before this
//    apply's own PrintObjects/regions are rebuilt - so it still describes the PREVIOUS apply.
// 2. Even the LATE normalize_fdm_2() call (after the rebuild) can itself still be premature:
//    further region/support processing between that point and the end of apply() can further
//    change the true used-filament count (confirmed on this exact config: the LATE pass saw 1
//    mid-apply, but a fresh read once apply() fully returns correctly reports 3). On a Print's
//    very first-ever apply, this transiently wrong "1" incorrectly forces enable_prime_tower
//    off - and normalize_fdm_2 never restores a value it has cleared, so that wrong "off" would
//    otherwise stick in m_config/m_full_print_config permanently, surfacing as a "fix" (a
//    spurious diff) on some later, unrelated apply of the same unchanged config.
//
// Fixed by (a) caching the fully-settled used-filament count from the true end of apply() in
// m_last_known_used_filament_count for the EARLY pass of the NEXT apply to read, and (b) a
// final self-correction pass at the end of apply() that re-derives enable_prime_tower /
// independent_support_layer_height from their pristine, as-specified values (captured before
// any normalize_fdm_2 mutation) using the now-fully-settled count, correcting anything the
// LATE pass got wrong before this same apply() call returns.
//
// A second, compounding issue surfaced once (a)+(b) stopped those two settings from flipping:
// tool_filament_map, filament_volume_map and filament_nozzle_map are engine-derived bookkeeping
// (Print::update_filament_maps_to_config()'s write-back, ToolOrdering.cpp, recomputes them from
// the grouping result on every process() in every filament_map_mode including Manual), never
// legitimate user input on a filament-mapping-enabled non-BBL printer - but Print::apply() still
// diffed them (via full_print_config_diffs, a separate diff pass from the main print_diff), so a
// reapply of the identical config still reported APPLY_STATUS_CHANGED. Fixed by excluding these
// three keys from both diff passes for this printer shape, mirroring the existing filament_map_2
// precedent; BBL/H2D printers, where filament_volume_map is genuine dialog-authored input, are
// explicitly excluded from that exclusion (see PrintApply.cpp for the exact gating).
TEST_CASE("Reapplying an unchanged mapped-printer config after slicing does not flip prime tower/support settings", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(5, {
        { "nozzle_diameter",               "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,3,4,1" },   // 5 filaments on 4 tools (1 and 5 share tool 1)
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "5" },
        { "enable_prime_tower",             "1" },
        { "independent_support_layer_height", "1" },
        { "filament_swap_gcode",            ";SWAP" },
        // Matches what init_print's internal config carries, so this reapply of the bare test
        // config differs from the print's actual first-apply config in nothing but what
        // Print::apply()'s own normalization might (wrongly) change - isolating the bug instead
        // of also picking up an incidental, unrelated mismatch between the two configs.
        { "gcode_comments",                 "1" },
    });
    Model model;
    Print print;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    print.process();
    const bool islh_before = print.config().independent_support_layer_height.value;
    const bool ept_before  = print.config().enable_prime_tower.value;

    // Re-apply the exact same, unchanged config again - matching the GUI's post-slice
    // background_process update, which always rebuilds from the current presets rather than
    // reusing the print's own already-resolved config.
    const auto status = print.apply(model, config);
    const bool islh_after = print.config().independent_support_layer_height.value;
    const bool ept_after  = print.config().enable_prime_tower.value;

    // Assert the strongest true form: a reapply of the exact same config, once a slice has
    // completed, must report no change at all - not just that these two settings held steady.
    // (Getting here also required excluding tool_filament_map/filament_volume_map/
    // filament_nozzle_map - engine-derived bookkeeping keys with no legitimate user-input path
    // on this printer shape - from Print::apply()'s diffing; see PrintApply.cpp.)
    INFO("status = " << (int)status);
    INFO("independent_support_layer_height: before=" << islh_before << " after=" << islh_after);
    INFO("enable_prime_tower: before=" << ept_before << " after=" << ept_after);
    CHECK(islh_before == islh_after);
    CHECK(ept_before == ept_after);
    CHECK((int)status == (int)PrintBase::APPLY_STATUS_UNCHANGED);
}
