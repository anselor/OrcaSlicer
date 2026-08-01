#ifndef slic3r_FilamentInventory_hpp_
#define slic3r_FilamentInventory_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// A single physical filament known to a printer: either the filament currently loaded into a
// tool (slot 0 of that tool's list, see FilamentInventory::tools) or a swappable filament
// recorded for later use on that tool.
struct PhysicalFilament
{
    int         id   = 0;        // stable per-machine id, > 0; 0 = invalid/unassigned
    std::string color;           // "#RRGGBB" (may be empty = unknown)
    std::string type;            // e.g. "PLA" (may be empty = unknown)
    enum class Kind { Manual, Mmu } kind = Kind::Manual; // Mmu reserved for future AMS-style slots (R3.7)
    bool empty() const { return color.empty() && type.empty(); }
};

// Alias retained for source compatibility with call sites not yet reworked to the multi-slot
// model (see the physical-filaments-merge plan); refers to the same type.
using LoadedFilament = PhysicalFilament;

// The set of physical filaments known to a printer, grouped by tool. Element 0 of each tool's
// list is always present and is the currently loaded filament for that tool (possibly empty());
// further elements are swappable filaments recorded for that tool.
struct FilamentInventory
{
    std::vector<std::vector<PhysicalFilament>> tools; // index = physical tool
    int next_id = 1;                                   // id allocator for new entries, persisted

    std::string serialize() const;
    // Tolerant: malformed or short input yields an inventory of one empty loaded slot per tool,
    // sized to tool_count. Also migrates the old v1 flat-array format (each entry becomes that
    // tool's loaded filament, with freshly assigned ids).
    static FilamentInventory deserialize(const std::string& s, size_t tool_count);

    // Lookup helpers used by dialog/engine plumbing.
    const PhysicalFilament* find(int id) const; // nullptr if absent
    int tool_of(int id) const;                  // 0-based tool index, -1 if absent
};

// True when nothing is recorded anywhere in the inventory (every slot, loaded and swappable, on
// every tool is empty()) -- a fresh machine profile that was never populated. Used both to decide
// whether the mapping dialog can target real physical filaments (spec R3.3) or must fall back to
// picking a bare tool (FilamentMapRowsPanel's bootstrap mode), and to gate FilamentMapDialog's
// "record these as loaded" offer: checking every slot (not just slot 0/loaded) matters because a
// tool can have an empty loaded slot but a real swappable one (cleared via the Physical Filaments
// editor's per-tool Clear button, which only touches slot 0) -- treating that as "all unset"
// would let the record write silently discard the swappable entry.
bool inventory_all_unset(const FilamentInventory& inv);

// Auto-match each used plate filament to the best compatible physical filament in the inventory
// (loaded filaments preferred over swappable ones at equal color distance).
// filament_colors/filament_types: full project list, 0-based by filament id.
// plate_filaments: 1-based used filament ids, in plate order.
// Returns one entry per plate filament (same order): the proposed physical filament id, or 0 for
// no compatible match. Two plate filaments may legitimately propose the same id -- that is a
// merge proposal, not an error.
std::vector<int> match_filaments_to_inventory(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<int>&         plate_filaments,
    const FilamentInventory&        inventory);

// Builds the mapping dialog's per-row proposal: one entry per plate_filaments entry (same
// order), used to seed FilamentMapRowsPanel. Meaning of an entry depends on whether the
// inventory has anything recorded (see inventory_all_unset):
//  - normal (inventory has real targets): the proposed physical filament id, or 0 for no
//    target. stored_physical_map (full-length, 0-based by filament id) is only consulted when
//    stored_map_confirmed is true (the plate has actually been through a confirmed manual
//    mapping before); a stored entry is only kept as-is when it still resolves to a real,
//    non-empty physical filament -- inventory.find(id) returning a slot whose empty() is true
//    (e.g. a loaded/swap slot the user cleared in the Physical Filaments editor after this
//    plate's map was saved, which keeps its id but drops its content) must fall through to
//    auto-match exactly like a missing id, not be treated as a valid stored target.
//  - bootstrap (inventory has nothing recorded anywhere): there is no physical filament to
//    target, so a confirmed row instead reproposes its previously-stored TOOL
//    (stored_filament_map, full-length 1-based project filament -> tool map), encoded as
//    -(tool) so it can never collide with a real (positive) physical id; 0 still means no
//    proposal. FilamentMapRowsPanel decodes this the same way it independently derives
//    bootstrap mode -- from this same inventory.
std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<int>&         plate_filaments,
    const std::vector<int>&         stored_physical_map,
    const std::vector<int>&         stored_filament_map,
    const FilamentInventory&        inventory,
    bool                             stored_map_confirmed);

} // namespace Slic3r

#endif // slic3r_FilamentInventory_hpp_
