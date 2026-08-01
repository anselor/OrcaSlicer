#ifndef slic3r_GUI_FilamentMapRowsPanel_hpp_
#define slic3r_GUI_FilamentMapRowsPanel_hpp_

#include "GUI.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/FilamentInventory.hpp"

#include <wx/bmpcbox.h>
#include <vector>
#include <string>

namespace Slic3r { namespace GUI {

// Orca: one-row-per-plate-filament mapping panel for opted-in non-BBL multi-tool printers,
// used as an alternative to the drag-drop FilamentMapToolsPanel. Each row shows a plate
// filament (color/id/type/name) next to a dropdown of the printer's physical filaments
// (loaded/swappable, grouped by tool), so the user picks a physical TARGET rather than a bare
// tool -- two rows landing on the same physical filament is a merge (spec R3.3/R3.4), not an
// error, and the footer reflects that.
class FilamentMapRowsPanel : public wxPanel
{
public:
    // filament_colors/filament_types/filament_names: full project lists, 0-based by filament id.
    // plate_filaments: 1-based used filament ids, in plate order.
    // proposal: one entry per plate_filaments entry, from compute_physical_map_proposal (see
    //   that function -- its doc explains the encoding this decodes): the proposed physical
    //   filament id (0 = unassigned) in normal mode, or -(1-based tool) in bootstrap mode (see
    //   `inventory` below). Which encoding applies is always consistent with this same
    //   `inventory` argument, since both this panel and compute_physical_map_proposal derive
    //   bootstrap-ness from it via the same Slic3r::inventory_all_unset.
    // inventory: printer's currently known physical filaments (loaded + swappable), used to
    //   build the dropdown contents. If the inventory has no recorded filament at all (a fresh
    //   machine profile never edited), the panel falls back to one dropdown entry per tool
    //   (bootstrap mode) so a first-time "record these as loaded" pass is still possible --
    //   see BuildTargetOptions.
    // tool_count: physical tool count.
    FilamentMapRowsPanel(wxWindow                        *parent,
                         const std::vector<std::string> &filament_colors,
                         const std::vector<std::string> &filament_types,
                         const std::vector<std::string> &filament_names,
                         const std::vector<int>          &plate_filaments,
                         const std::vector<int>          &proposal,
                         const FilamentInventory          &inventory,
                         size_t                            tool_count);

    // Full-length 1-based filament->tool passthrough map for filaments not on this plate; also
    // used as the tool for a bootstrap-mode row (see GetFilamentMaps). Defaults to 1 if never
    // called. Call before GetFilamentMaps().
    void SetBaseMap(const std::vector<int> &full_map);

    // Full-length physical-filament-id passthrough map for filaments not on this plate (0 =
    // unassigned). Defaults to all-0 if never called. Call before GetPhysicalMaps().
    void SetBasePhysicalMap(const std::vector<int> &full_map);

    // Full-length 1-based filament->tool map, DERIVED per plate row from the chosen physical
    // filament's owning tool (inventory.tool_of(id)+1); a bootstrap-mode row (no physical
    // filament exists yet) reports its raw tool pick directly instead. Every other filament
    // keeps SetBaseMap()'s passthrough value.
    std::vector<int> GetFilamentMaps() const;

    // Full-length physical filament id per project filament (0 = unassigned/not on this plate's
    // rows); a bootstrap-mode row always reports 0, since no physical filament exists for it yet
    // (see FilamentMapDialog::on_ok's record-to-inventory follow-up, which mints one and can then
    // backfill this). Every other filament keeps SetBasePhysicalMap()'s passthrough value.
    std::vector<int> GetPhysicalMaps() const;

    // True when every row has a target selected.
    bool AllRowsAssigned() const;

    // Sum over tools of max(0, distinct-targets-on-that-tool - 1): how many tool swaps this
    // plate will need. Rows sharing the same physical filament on a tool count as one target
    // (a merge), not a swap. In bootstrap mode (see BuildTargetOptions) every row is its own
    // target, so this reduces to the tool-index-only count.
    int SwapCount() const;

private:
    struct Row
    {
        int                filament_id; // 1-based
        wxBitmapComboBox  *tool_choice;
        Label             *warning;
    };

    // One dropdown entry: either a real physical filament (id > 0, tool = its owning tool) or,
    // in bootstrap mode, a plain per-tool placeholder (id == 0, tool = that tool's index).
    // Shared by every row -- the same options, in the same order, appear in every row's
    // wxBitmapComboBox, so a row's GetSelection() index is also this vector's index.
    struct TargetOption
    {
        int      id;   // physical filament id, or 0 for a bootstrap placeholder
        int      tool; // 0-based owning/placeholder tool index
        wxString label;
        wxBitmap bitmap;
    };

    // Populates m_target_options and m_bootstrap_mode from the inventory: one entry per
    // non-empty physical filament (loaded slot first, then swappable), grouped/labeled by tool;
    // or, when the inventory has no recorded filament at all, one plain "Tool N" entry per tool
    // (bootstrap mode -- see the class comment and GetPhysicalMaps).
    void BuildTargetOptions(const FilamentInventory &inventory);
    void BuildToolChoices(wxBitmapComboBox *choice) const;
    void OnRowChanged(wxCommandEvent &event);
    // Recomputes and applies the footer label text only; does not fire wxEVT_INVALID_MANUAL_MAP.
    // Used both after each row change and once at the end of construction, since the
    // constructor itself must stay silent: the owning dialog seeds its own initial OK state
    // by querying AllRowsAssigned() right after construction, rather than reacting to an event.
    void UpdateFooter();

    // Resolves a row's current selection to (tool, physical id) via m_target_options. Returns
    // false (leaving tool/id untouched) when the row has no selection.
    bool ResolveRow(const Row &row, int &tool, int &id) const;

    // Shared by UpdateFooter (footer text) and SwapCount (its own totals): idle tool count,
    // swap count (extra distinct targets per tool beyond the first), and merged-row count
    // (extra rows sharing a physical id beyond the first, 0 in bootstrap mode).
    void ComputeStats(int &idle, int &swaps, int &merged) const;

    std::vector<Row>          m_rows;
    std::vector<TargetOption> m_target_options;
    bool                      m_bootstrap_mode{false};
    std::vector<int>          m_base_map;
    std::vector<int>          m_base_physical_map;
    size_t                    m_filament_count;
    size_t                    m_tool_count;

    Label *m_footer{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentMapRowsPanel_hpp_
