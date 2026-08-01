#include "FilamentInventory.hpp"
#include "FlushVolPredictor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_set>

namespace Slic3r {

using json = nlohmann::json;

// Colors more than this ΔE2000 apart are treated as different filaments even when the type
// matches, so an auto-match can legitimately report "no compatible physical filament" (0)
// instead of always grabbing the closest same-type slot. Chosen well above real-world color
// jitter (near-duplicate hex codes measure well under 5) and well below a same-type slot of an
// unrelated color (measured ~34-93 for the swatches exercised by the unit tests).
static constexpr float kMaxAutoMatchColorDistance = 20.f;

// A slot or plate filament with unknown color still qualifies on type alone, but is ranked at
// the edge of the cutoff (least-preferred-but-still-qualifying) so it never outranks a slot with
// a real, close color match — it's only chosen when no better (known-color) candidate exists.
static constexpr float kUnknownColorDistance = kMaxAutoMatchColorDistance - 0.001f;

static bool parse_hex_color(const std::string& s, FlushPredict::RGBColor& out)
{
    if (s.size() != 7 || s[0] != '#')
        return false;
    try {
        out.r = static_cast<unsigned char>(std::stoi(s.substr(1, 2), nullptr, 16));
        out.g = static_cast<unsigned char>(std::stoi(s.substr(3, 2), nullptr, 16));
        out.b = static_cast<unsigned char>(std::stoi(s.substr(5, 2), nullptr, 16));
    } catch (...) {
        return false;
    }
    return true;
}

static const char* kind_to_string(PhysicalFilament::Kind kind)
{
    return kind == PhysicalFilament::Kind::Mmu ? "mmu" : "manual";
}

static PhysicalFilament::Kind kind_from_string(const std::string& s)
{
    return s == "mmu" ? PhysicalFilament::Kind::Mmu : PhysicalFilament::Kind::Manual;
}

std::string FilamentInventory::serialize() const
{
    json jtools = json::array();
    for (const auto& tool : tools) {
        json jtool = json::array();
        for (const auto& pf : tool)
            jtool.push_back({ {"id", pf.id}, {"color", pf.color}, {"type", pf.type}, {"kind", kind_to_string(pf.kind)} });
        jtools.push_back(std::move(jtool));
    }
    json j;
    j["next_id"] = next_id;
    j["tools"]   = std::move(jtools);
    return j.dump();
}

FilamentInventory FilamentInventory::deserialize(const std::string& s, size_t tool_count)
{
    auto empty_inventory = [tool_count]() {
        FilamentInventory inv;
        inv.tools.assign(tool_count, std::vector<PhysicalFilament>(1));
        inv.next_id = 1;
        return inv;
    };

    json j;
    try {
        j = json::parse(s);
    } catch (...) {
        return empty_inventory();
    }

    if (j.is_array()) {
        // v1 migration: flat array of {color,type}, one per tool -> each becomes that tool's
        // loaded filament (slot 0), assigned a freshly minted id unconditionally by position
        // (id == index + 1), even for an empty entry -- unlike the GUI write-path shims
        // (FilamentInventoryEditor::on_ok, FilamentMapDialog::on_ok), which only mint an id for
        // a non-empty slot. This path assigns per the brief's literal "ids 1..N" contract for a
        // one-time migration; the GUI shims avoid handing out ids nothing occupies since they
        // rebuild the inventory from scratch on every save (see their own comments).
        FilamentInventory inv;
        inv.tools.assign(tool_count, std::vector<PhysicalFilament>(1));
        size_t migrated = 0;
        for (; migrated < tool_count && migrated < j.size(); ++migrated) {
            if (!j[migrated].is_object())
                continue;
            PhysicalFilament pf;
            pf.id = static_cast<int>(migrated) + 1;
            if (j[migrated].contains("color") && j[migrated]["color"].is_string())
                pf.color = j[migrated]["color"].get<std::string>();
            if (j[migrated].contains("type") && j[migrated]["type"].is_string())
                pf.type = j[migrated]["type"].get<std::string>();
            inv.tools[migrated][0] = pf;
        }
        inv.next_id = static_cast<int>(migrated) + 1;
        return inv;
    }

    if (!j.is_object() || !j.contains("tools") || !j["tools"].is_array())
        return empty_inventory();

    FilamentInventory inv;
    inv.tools.assign(tool_count, std::vector<PhysicalFilament>(1));
    const json& jtools = j["tools"];
    for (size_t i = 0; i < tool_count && i < jtools.size(); ++i) {
        if (!jtools[i].is_array())
            continue;
        std::vector<PhysicalFilament> slots;
        for (const auto& jpf : jtools[i]) {
            if (!jpf.is_object())
                continue;
            PhysicalFilament pf;
            if (jpf.contains("id") && jpf["id"].is_number_integer())
                pf.id = jpf["id"].get<int>();
            if (jpf.contains("color") && jpf["color"].is_string())
                pf.color = jpf["color"].get<std::string>();
            if (jpf.contains("type") && jpf["type"].is_string())
                pf.type = jpf["type"].get<std::string>();
            if (jpf.contains("kind") && jpf["kind"].is_string())
                pf.kind = kind_from_string(jpf["kind"].get<std::string>());
            slots.push_back(pf);
        }
        if (slots.empty())
            slots.resize(1); // slot 0 (loaded) must always be present
        inv.tools[i] = std::move(slots);
    }

    // Reconcile ids from untrusted input: find()/tool_of() key on id, so a duplicate would make
    // one entry unreachable, and a next_id at or below an id already in use would let the
    // allocator (Task 6) mint a fresh collision the moment it's used. Renumber the colliding or
    // missing/invalid (<=0) id via the allocator rather than dropping the entry -- that preserves
    // the caller's color/type data. Empty slots (nothing recorded) keep whatever id parsed,
    // including 0/absent; they carry no data to collide over.
    int max_id = 0;
    for (const auto& tool : inv.tools)
        for (const auto& pf : tool)
            max_id = std::max(max_id, pf.id);
    int parsed_next_id = (j.contains("next_id") && j["next_id"].is_number_integer()) ? j["next_id"].get<int>() : 1;
    int next_id = std::max(1, std::max(parsed_next_id, max_id + 1));

    std::unordered_set<int> seen_ids;
    for (auto& tool : inv.tools) {
        for (auto& pf : tool) {
            if (pf.empty())
                continue;
            if (pf.id > 0 && seen_ids.insert(pf.id).second)
                continue; // first time we've seen this id; keep it
            pf.id = next_id++;
            seen_ids.insert(pf.id);
        }
    }
    inv.next_id = next_id;
    return inv;
}

const PhysicalFilament* FilamentInventory::find(int id) const
{
    if (id <= 0)
        return nullptr;
    for (const auto& tool : tools)
        for (const auto& pf : tool)
            if (pf.id == id)
                return &pf;
    return nullptr;
}

int FilamentInventory::tool_of(int id) const
{
    if (id <= 0)
        return -1;
    for (size_t ti = 0; ti < tools.size(); ++ti)
        for (const auto& pf : tools[ti])
            if (pf.id == id)
                return static_cast<int>(ti);
    return -1;
}

std::vector<int> match_filaments_to_inventory(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<int>&         plate_filaments,
    const FilamentInventory&        inventory)
{
    std::vector<int> result(plate_filaments.size(), 0);
    const size_t num_tools = inventory.tools.size();
    if (num_tools == 0)
        return result;

    struct Candidate { size_t fi; int id; float dist; bool loaded; };
    std::vector<Candidate> candidates;

    for (size_t fi = 0; fi < plate_filaments.size(); ++fi) {
        int filament_id = plate_filaments[fi] - 1; // stored 1-based, arrays are 0-based
        if (filament_id < 0 || static_cast<size_t>(filament_id) >= filament_colors.size() ||
            static_cast<size_t>(filament_id) >= filament_types.size())
            continue;
        const std::string& f_color = filament_colors[filament_id];
        const std::string& f_type  = filament_types[filament_id];

        for (size_t ti = 0; ti < num_tools; ++ti) {
            const auto& slots = inventory.tools[ti];
            for (size_t si = 0; si < slots.size(); ++si) {
                const PhysicalFilament& pf = slots[si];
                if (pf.empty()) // nothing physically recorded there
                    continue;
                if (!f_type.empty() && !pf.type.empty() && f_type != pf.type)
                    continue; // type mismatch disqualifies

                float dist = kUnknownColorDistance; // color unknown on one side; see constant above
                FlushPredict::RGBColor fc, sc;
                if (parse_hex_color(f_color, fc) && parse_hex_color(pf.color, sc)) {
                    dist = FlushPredict::calc_color_distance(fc, sc);
                    if (dist > kMaxAutoMatchColorDistance)
                        continue; // colors too far apart to be considered a match
                }
                candidates.push_back({fi, pf.id, dist, si == 0});
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.dist != b.dist)
            return a.dist < b.dist;
        return a.loaded && !b.loaded; // loaded filaments win ties over swappable ones
    });

    // Phase 1: exclusive assignment, each physical filament used at most once.
    std::vector<bool> filament_assigned(plate_filaments.size(), false);
    std::unordered_set<int> id_used;
    for (const auto& c : candidates) {
        if (filament_assigned[c.fi] || id_used.count(c.id))
            continue;
        result[c.fi] = c.id;
        filament_assigned[c.fi] = true;
        id_used.insert(c.id);
    }

    // Phase 2: only after every filament has had its exclusive shot, let leftover filaments
    // reuse an already-assigned physical filament (closest compatible one first). This is a
    // legal merge proposal, not an error -- see match_filaments_to_inventory's contract.
    for (const auto& c : candidates) {
        if (filament_assigned[c.fi])
            continue;
        result[c.fi] = c.id;
        filament_assigned[c.fi] = true;
    }

    return result;
}

bool inventory_all_unset(const FilamentInventory& inv)
{
    for (const auto& tool : inv.tools)
        for (const auto& pf : tool)
            if (!pf.empty())
                return false;
    return true;
}

std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<int>&         plate_filaments,
    const std::vector<int>&         stored_physical_map,
    const std::vector<int>&         stored_filament_map,
    const FilamentInventory&        inventory,
    bool                             stored_map_confirmed)
{
    // Orca: bootstrap (nothing recorded anywhere) has no physical filament to propose -- a
    // confirmed row instead reproposes its previously-stored TOOL, sentinel-encoded as
    // -(tool) so FilamentMapRowsPanel (which derives the same bootstrap/not split from this
    // same inventory) can tell it apart from a real (positive) physical id. This is the fix for
    // a plate confirmed via bootstrap with the "record as loaded" offer left unchecked: its
    // physical map stays all-0 forever (nothing was ever recorded to point at), so without this
    // fallback every reopen would silently reset every row instead of honoring filament_map.
    if (inventory_all_unset(inventory)) {
        const int tool_count = (int) inventory.tools.size();
        std::vector<int> proposal(plate_filaments.size(), 0);
        for (size_t i = 0; i < plate_filaments.size(); ++i) {
            int f    = plate_filaments[i];
            int tool = (stored_map_confirmed && f >= 1 && f <= (int) stored_filament_map.size()) ? stored_filament_map[f - 1] : 0;
            proposal[i] = (tool >= 1 && tool <= tool_count) ? -tool : 0;
        }
        return proposal;
    }

    std::vector<int> matched = match_filaments_to_inventory(filament_colors, filament_types, plate_filaments, inventory);

    std::vector<int> proposal(plate_filaments.size(), 0);
    for (size_t i = 0; i < plate_filaments.size(); ++i) {
        int f      = plate_filaments[i];
        int stored = (stored_map_confirmed && f >= 1 && f <= (int) stored_physical_map.size()) ? stored_physical_map[f - 1] : 0;
        const PhysicalFilament* pf = stored > 0 ? inventory.find(stored) : nullptr;
        bool stored_valid = pf != nullptr && !pf->empty();
        proposal[i] = stored_valid ? stored : (i < matched.size() ? matched[i] : 0);
    }
    return proposal;
}

} // namespace Slic3r
