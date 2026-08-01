#include <catch2/catch_test_macros.hpp>
#include "libslic3r/FilamentInventory.hpp"
using namespace Slic3r;

TEST_CASE("Inventory v2 round-trips ids, kinds, and next_id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools.resize(2);
    inv.tools[0] = { {1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"} }; // loaded red + swappable yellow
    inv.tools[1] = { {2, "#FFFFFF", "PETG"} };                       // loaded white
    inv.next_id = 6;

    FilamentInventory back = FilamentInventory::deserialize(inv.serialize(), 2);
    REQUIRE(back.next_id == 6);
    REQUIRE(back.tools.size() == 2);
    REQUIRE(back.tools[0].size() == 2);
    REQUIRE(back.tools[0][0].id == 1);
    REQUIRE(back.tools[0][0].color == "#FF0000");
    REQUIRE(back.tools[0][0].kind == PhysicalFilament::Kind::Manual);
    REQUIRE(back.tools[0][1].id == 5);
    REQUIRE(back.tools[0][1].color == "#FFFF00");
    REQUIRE(back.tools[1].size() == 1);
    REQUIRE(back.tools[1][0].id == 2);
    REQUIRE(back.tools[1][0].type == "PETG");
}

TEST_CASE("Deserialize dedupes colliding v2 ids and floors next_id above the max", "[FilamentInventory]") {
    // Two slots claim the same id 1 -- the later one must be renumbered, not silently shadowed
    // (find()/tool_of() key on id, so a collision would make one entry unreachable).
    std::string v2 = R"({"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}],[{"id":1,"color":"#FFFFFF","type":"PETG"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 2);
    REQUIRE(inv.tools[0][0].id == 1);
    REQUIRE(inv.tools[1][0].id != 1);
    REQUIRE(inv.tools[1][0].id > 0);
    REQUIRE(inv.tools[0][0].color == "#FF0000");   // data preserved, not dropped
    REQUIRE(inv.tools[1][0].color == "#FFFFFF");
    REQUIRE(inv.next_id > inv.tools[1][0].id);
    REQUIRE(inv.next_id > 1);
}

TEST_CASE("Deserialize clamps a stale next_id above every id already in use", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"id":7,"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.tools[0][0].id == 7);
    REQUIRE(inv.next_id > 7);
}

TEST_CASE("Deserialize assigns a fresh id to a non-empty v2 slot missing its id", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.tools[0][0].id > 0);
    REQUIRE(inv.next_id > inv.tools[0][0].id);
}

TEST_CASE("Deserialize tolerates a v2 object missing the tools key", "[FilamentInventory]") {
    FilamentInventory inv = FilamentInventory::deserialize(R"({"next_id":5})", 3);
    REQUIRE(inv.tools.size() == 3);
    for (const auto& t : inv.tools) {
        REQUIRE(t.size() == 1);
        REQUIRE(t[0].empty());
    }
    REQUIRE(inv.next_id == 1);
}

TEST_CASE("Inventory migrates the old v1 flat-array format", "[FilamentInventory]") {
    std::string v1 = R"([{"color":"#FF0000","type":"PLA"},{"color":"#00FF00","type":"PETG"}])";
    FilamentInventory inv = FilamentInventory::deserialize(v1, 2);
    REQUIRE(inv.tools.size() == 2);
    REQUIRE(inv.tools[0].size() == 1);
    REQUIRE(inv.tools[0][0].id == 1);
    REQUIRE(inv.tools[0][0].color == "#FF0000");
    REQUIRE(inv.tools[0][0].type == "PLA");
    REQUIRE(inv.tools[1].size() == 1);
    REQUIRE(inv.tools[1][0].id == 2);
    REQUIRE(inv.tools[1][0].color == "#00FF00");
    REQUIRE(inv.next_id == 3);
}

TEST_CASE("Inventory tolerates junk and sizes to tool_count", "[FilamentInventory]") {
    FilamentInventory junk = FilamentInventory::deserialize("not json{", 3);
    REQUIRE(junk.tools.size() == 3);
    for (const auto& t : junk.tools) {
        REQUIRE(t.size() == 1);
        REQUIRE(t[0].empty());
    }
    REQUIRE(junk.next_id == 1);
}

TEST_CASE("Matcher assigns by color within type and flags no-match", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {
        {{1, "#FF0000", "PLA"}},
        {{2, "#FFFFFF", "PLA"}},
        {{3, "#000000", "PLA"}},
        {{4, "#00FF00", "PETG"}},
    };
    std::vector<std::string> colors = {"#FE0102", "#111111", "#00EE00"}; // near-red, near-black, near-green
    std::vector<std::string> types  = {"PLA", "PLA", "PLA"};
    std::vector<int> plate = {1, 2, 3};   // 1-based
    std::vector<int> res = match_filaments_to_inventory(colors, types, plate, inv);
    REQUIRE(res.size() == 3);
    REQUIRE(res[0] == 1);    // near-red -> id 1 (red PLA)
    REQUIRE(res[1] == 3);    // near-black -> id 3 (black PLA)
    REQUIRE(res[2] == 0);    // green PLA: only green slot is PETG -> no match
}

TEST_CASE("Matcher reuses a physical filament only when demand outnumbers compatible slots", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#FFFFFF", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000", "#FFFFFF", "#F0F0F0"};
    std::vector<std::string> types  = {"PLA", "PLA", "PLA"};
    std::vector<int> plate = {1, 2, 3};
    std::vector<int> res = match_filaments_to_inventory(colors, types, plate, inv);
    REQUIRE(res[0] == 1);
    REQUIRE(res[1] == 2);
    REQUIRE(res[2] == 2);    // overflow shares the closest compatible physical filament -- a legal merge proposal
}

TEST_CASE("Empty inventory yields all no-match", "[FilamentInventory]") {
    FilamentInventory inv; inv.tools.resize(4); // each tool has zero physical filaments
    std::vector<int> res = match_filaments_to_inventory({"#FF0000"}, {"PLA"}, {1}, inv);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("Matcher prefers a known color match over a type-only slot", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "", "PLA"}}}; // exact color match, and an unknown-color PLA slot
    std::vector<int> res = match_filaments_to_inventory({"#FF0000"}, {"PLA"}, {1}, inv);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0] == 1); // the known, exact color match must win over the unknown-color slot
}

TEST_CASE("Matcher accepts a type-only slot when it is the only compatible one", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "", "PLA"}}}; // color unknown, but type matches and nothing else is available
    std::vector<int> res = match_filaments_to_inventory({"#123456"}, {"PLA"}, {1}, inv);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0] == 1);
}

TEST_CASE("Matcher prefers a loaded filament over an equally-close swappable one", "[FilamentInventory]") {
    FilamentInventory inv;
    // Tool 0: loaded red PLA (id 1). Tool 1: nothing loaded, but a swappable red PLA (id 5).
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "", ""}, {5, "#FF0000", "PLA"}}};
    std::vector<int> res = match_filaments_to_inventory({"#FF0000"}, {"PLA"}, {1}, inv);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0] == 1); // loaded wins over swappable at equal distance
}

TEST_CASE("Two plate filaments may propose the same physical id as a merge", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000", "#FE0101"}; // both near-identical red
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> plate = {1, 2};
    std::vector<int> res = match_filaments_to_inventory(colors, types, plate, inv);
    REQUIRE(res[0] == 1);
    REQUIRE(res[1] == 1); // legal merge proposal, not an error
}

TEST_CASE("find and tool_of look up physical filaments by id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"}}, {{2, "#FFFFFF", "PETG"}}};
    const PhysicalFilament* pf = inv.find(5);
    REQUIRE(pf != nullptr);
    REQUIRE(pf->color == "#FFFF00");
    REQUIRE(inv.tool_of(5) == 0);
    REQUIRE(inv.tool_of(2) == 1);
    REQUIRE(inv.find(99) == nullptr);
    REQUIRE(inv.tool_of(99) == -1);
    REQUIRE(inv.find(0) == nullptr);
}

TEST_CASE("Proposal keeps a confirmed stored id that still resolves to a non-empty physical filament", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#00FF00", "PLA"}}};
    std::vector<std::string> colors = {"#0000FF"}; // would not auto-match either slot
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 0);
    stored[0] = 2; // filament id 1 (0-based index 0) stored as physical id 2
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{2});
}

TEST_CASE("Proposal re-matches when the stored id points at a slot the editor cleared", "[FilamentInventory]") {
    // Carried-forward Task 6 hazard: a cleared slot keeps its live id but drops its content, so
    // find(id) succeeds while empty() is true -- that must still fall through to auto-match,
    // not be trusted as a valid stored target.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "", ""}}}; // tool 1's slot 0 was cleared
    std::vector<std::string> colors = {"#FE0101"}; // near-red, should auto-match id 1
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 2); // stale reference to the now-cleared slot
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{1}); // re-matched, not the stale cleared id
}

TEST_CASE("Proposal re-matches when the stored id no longer exists in the inventory", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<std::string> colors = {"#FE0101"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 99); // id no longer present (e.g. removed via the editor)
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{1});
}

TEST_CASE("Proposal ignores the stored map entirely on an unconfirmed (fresh) plate", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#00FF00", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000"}; // matches id 1 exactly
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 2); // present and valid, but the plate was never actually confirmed
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, stored, {}, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{1}); // auto-match wins, stored id 2 is not trusted
}

TEST_CASE("Proposal falls back to no-match when auto-match also fails", "[FilamentInventory]") {
    // Every slot is genuinely empty here (not the inventory_all_unset/bootstrap case below --
    // matcher still runs and legitimately finds nothing compatible).
    FilamentInventory inv;
    inv.tools = {{{1, "", ""}}, {{2, "", ""}}}; // ids present, but nothing recorded
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, {0}, {}, inv, false);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("inventory_all_unset is true only when every slot on every tool is empty", "[FilamentInventory]") {
    FilamentInventory empty_inv; empty_inv.tools.resize(2);
    REQUIRE(inventory_all_unset(empty_inv));

    FilamentInventory loaded_inv; loaded_inv.tools = {{{1, "#FF0000", "PLA"}}, {}};
    REQUIRE_FALSE(inventory_all_unset(loaded_inv));

    // The hazard this exists to catch: tool 0's loaded slot (index 0) is empty, but a swappable
    // slot on the same tool (index 1) still carries real data -- a tool-level/loaded-only check
    // would misreport this as "all unset".
    FilamentInventory swap_only_inv;
    swap_only_inv.tools = {{{0, "", ""}, {5, "#00FF00", "PETG"}}};
    REQUIRE_FALSE(inventory_all_unset(swap_only_inv));
}

TEST_CASE("Proposal reproposes the stored tool (not a physical id) on a bootstrap reopen", "[FilamentInventory]") {
    // Regression: a plate confirmed via bootstrap mode (inventory was entirely empty when first
    // mapped) with the "record as loaded" offer left unchecked keeps an all-0 physical map
    // forever -- there was never anything to record it against. Reopening such a plate must
    // still honor its confirmed tool-level filament_map, not silently reset every row.
    FilamentInventory inv; inv.tools.resize(2); // still entirely empty -- bootstrap
    std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> plate = {1, 2}; // two plate filaments, 1-based ids
    std::vector<int> stored_physical(2, 0); // never recorded -- see the scenario above
    std::vector<int> stored_filament = {2, 1}; // filament 1 -> tool 2, filament 2 -> tool 1
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, stored_physical, stored_filament, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{-2, -1}); // sentinel-encoded tool picks, not physical ids
}

TEST_CASE("Bootstrap reopen ignores the stored tool when the plate was never confirmed", "[FilamentInventory]") {
    FilamentInventory inv; inv.tools.resize(2);
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored_filament = {2};
    std::vector<int> res = compute_physical_map_proposal(colors, types, plate, {0}, stored_filament, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{0});
}
