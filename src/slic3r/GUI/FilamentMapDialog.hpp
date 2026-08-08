#ifndef slic3r_FilamentMapDialog_hpp_
#define slic3r_FilamentMapDialog_hpp_

#include "FilamentMapPanel.hpp"
#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <vector>
#include <string>
#include <map>
#include <optional>
#include "CapsuleButton.hpp"
#include "Widgets/CheckBox.hpp"

class Button;
class wxScrolledWindow;
class wxBoxSizer;

namespace Slic3r {
class DynamicPrintConfig;

namespace GUI {
class DragDropPanel;
class Plater;
class PartPlate;
class SmartFilamentPanel;
class FilamentMapRowsPanel;
class ThumbnailPanel;

/**
 * @brief Try to pop up the filament map dialog before slicing.
 * 
 * Only pop up in multi extruder machines. If user don't want the pop up, we
 * pop up if the applied filament map mode in manual
 * 
 * @param is_slice_all  In slice all
 * @param plater_ref Plater to get/set global filament map
 * @param partplate_ref Partplate to get/set plate filament map mode
 * @return whether continue slicing
*/
bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up = false);

// Orca: send-time mapping for device-owned-protocol printers -- opens the row-mode dialog with
// no PartPlate to read a stored map from or persist a result onto (stored maps ignored; every
// row proposed straight from the inventory auto-match). Callers keep their own protocol gate and
// their own destination for the result. Returns std::nullopt if the user cancelled the dialog --
// callers must treat that as "abort the send", since the caller's own protocol check already
// established a mapping IS needed before this is called.
std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title);


class FilamentMapDialog : public wxDialog
{
    enum PageType {
        ptAuto,
        ptManual,
        ptDefault
    };
public:
    FilamentMapDialog(wxWindow *parent,
        const std::vector<std::string>& filament_color,
        const std::vector<std::string>& filament_type,
        const std::vector<int> &filament_map,
        const std::vector<int> &filament_volume_map,
        const std::vector<int> &filaments,
        const FilamentMapMode mode,
        bool machine_synced,
        bool show_default=true,
        bool with_checkbox = false
    );

    // Orca: rows-mode constructor for opted-in non-BBL multi-tool printers. Builds only the
    // title, a one-row-per-plate-filament FilamentMapRowsPanel (proposed from the printer's
    // physical-filament inventory), an "Edit physical filaments..." link, an optional "Remember
    // these..." checkbox, and the OK/Cancel row -- no mode switch, no auto/default pages.
    // get_mode() always reports fmmManual in this mode. Each row now targets a physical
    // filament, not a bare tool; the tool-level filament_map is derived from it
    // and stays the engine/3MF-persisted routing truth.
    // filament_color/filament_type/filament_names: full project lists, 0-based by filament id.
    // filament_map: full-length 1-based stored filament->tool map, used only as the base-map
    //   passthrough for filaments not on this plate (and as a bootstrap-mode row's tool -- see
    //   FilamentMapRowsPanel).
    // physical_filament_map: full-length stored filament->physical-filament-id map (0 =
    //   unassigned). Only consulted (via compute_physical_map_proposal) when
    //   filament_map_confirmed is true; entries that still resolve to a real, non-empty
    //   physical filament are kept as the row's proposal, everything else (unset/stale/cleared)
    //   is filled from auto_map_filaments (the inventory auto-match).
    //   When filament_map_confirmed is false (a
    //   plate's first-ever visit to this dialog) the stored map is entirely ignored and every
    //   row is proposed straight from the inventory match.
    // plate_filaments: 1-based used filament ids, in plate order.
    FilamentMapDialog(wxWindow                       *parent,
                      const std::vector<std::string> &filament_color,
                      const std::vector<std::string> &filament_type,
                      const std::vector<std::string> &filament_names,
                      const std::vector<int>          &filament_map,
                      const std::vector<int>          &physical_filament_map,
                      const std::vector<int>          &plate_filaments,
                      const FilamentInventory          &inventory,
                      size_t                           tool_count,
                      const std::string               &printer_preset_name,
                      bool                             filament_map_confirmed,
                      const wxString                  &title);

    // Orca: rows mode (m_rows_panel set) normally reports fmmManual -- OK saves a frozen map.
    // One exception: while m_live_auto_pending is armed (see
    // its doc), this reports fmmDefault instead, so the caller (show_filament_map_rows_dialog_
    // for_plate) saves the plate in live (re-mapped every slice) mode rather than a frozen one.
    FilamentMapMode get_mode();
    std::vector<int> get_filament_maps() const {
        if (m_rows_panel)
            return m_filament_map;
        if (m_page_type == PageType::ptManual)
            return m_filament_map;
        return {};
    }

    // Orca: physical filament id per project filament (0 = unassigned); only meaningful in
    // rows mode (the second, physical-targeting map, alongside the tool map).
    std::vector<int> get_physical_filament_maps() const {
        if (m_rows_panel)
            return m_physical_filament_map;
        return {};
    }

    std::vector<int> get_filament_volume_maps() const {
        if (m_rows_panel)
            return {};
        if (m_page_type == PageType::ptManual)
            return m_filament_volume_map;
        return {};
    }

    int ShowModal();
    void set_modal_btn_labels(const wxString& left_label, const wxString& right_label);

private:
    void on_ok(wxCommandEvent &event);
    void on_cancel(wxCommandEvent &event);
    void on_switch_mode(wxCommandEvent &event);
    void on_checkbox(wxCommandEvent &event);
    void on_edit_inventory(wxCommandEvent &event);
    // "Reset" restores the rows to the selections this dialog was opened with, and keeps the
    // dialog open (see on_apply_automatic's doc for returning a plate to live/auto mode).
    void on_reset_to_initial(wxCommandEvent &event);
    // Fills the rows with a freshly computed auto-match (ignoring any stored/confirmed map,
    // same as a first-ever bootstrap visit) so the user can review/adjust it before OK. Stays
    // open; never touches the plate. Pressing this button arms
    // m_live_auto_pending (see its doc) -- OK afterward, with no manual tile pick in between,
    // now saves the plate in live (re-mapped every slice) mode instead of a frozen one; any
    // manual tile pick after this button disarms it again and OK reverts to the frozen save.
    void on_apply_automatic(wxCommandEvent &event);

    void update_panel_status(PageType page);

    // The inventory (in m_store) this dialog's printer preset resolves to. Goes through
    // current_inventory_for_preset every time (never caches it) because resolution isn't
    // persisted to disk on its own -- callers only save when they actually have new data to write
    // (see on_ok's record-to-inventory backfill) -- so a fresh load_filament_inventories() reload
    // can still be missing this preset's entry.
    FilamentInventory& device();

    // Orca: (re)builds m_rows_panel from `proposal` (one entry per m_plate_filaments, physical
    // filament id or 0 for unassigned), wrapping it in a scrolled window when the row count is
    // large. Used by the constructor, on_edit_inventory() (after the inventory changes),
    // on_reset_to_initial(), and on_apply_automatic(). auto_matched is
    // forwarded as-is to FilamentMapRowsPanel's own auto_matched_proposal constructor argument --
    // see its doc for what it seeds on each row.
    void rebuild_rows_panel(const std::vector<int> &proposal, bool auto_matched);
    // Seeds OK-enabled and the "Remember..." checkbox visibility from the rows panel's and
    // inventory's current state. m_rows_panel's own construction stays silent (no
    // wxEVT_INVALID_MANUAL_MAP), so callers must invoke this explicitly after (re)building it.
    void seed_rows_mode_status();

    // Orca: id -> resolved preset display name (resolve_slot_preset) for every non-empty slot in
    // m_inventory; passed into FilamentMapRowsPanel's target-option labels. Needs the GUI's
    // PresetBundle, so it lives here rather than in the panel.
    std::map<int, std::string> build_slot_preset_names() const;

    // Compares each merged pair of project filaments (same GetPhysicalMaps() physical id)
    // against that slot's resolved preset and surfaces the first mismatch, if any, as a
    // non-blocking footer warning on m_rows_panel. Called from seed_rows_mode_status() so it
    // stays in sync with the same construction/row-change/edit-inventory paths that already
    // recompute the merged-row footer.
    void update_mismatch_warning();

    // Orca: re-renders m_preview_panel from m_preview_lit/m_preview_no_light, re-colored
    // by the dialog's current row picks -- a simplified single-view (no twin/original side) take
    // on SyncAmsInfoDialog's "after mapping" thumbnail; see the .cpp for exactly what's
    // simplified away. Called from seed_rows_mode_status() so every path that can change a row's
    // selection (construction, a tile pick, Reset, Automatic, editing the inventory) keeps it in
    // sync without a dedicated call at each site.
    // Orca: pure pixel work -- it drives NO GL render (it used to call
    // Plater::update_all_plate_thumbnails, which is an X BadMatch risk from a modal dialog; see
    // refresh_plate_thumbnails_for_preview in the .cpp). Hides the preview when no snapshot exists.
    void update_preview();

 private:
    FilamentMapManualPanel* m_manual_map_panel{nullptr};
    FilamentMapAutoPanel* m_auto_map_panel{nullptr};
    FilamentMapDefaultPanel* m_default_map_panel{nullptr};

    CapsuleButton* m_auto_btn{nullptr};
    CapsuleButton* m_manual_btn{nullptr};
    CapsuleButton* m_default_btn{nullptr};

    Button* m_ok_btn{nullptr};
    Button* m_cancel_btn{nullptr};
    CheckBox* m_checkbox{nullptr};
    SmartFilamentPanel* m_smart_filament{nullptr};

    // Orca: only set by the rows-mode constructor; when non-null every shared method
    // (get_mode/get_filament_maps/update_panel_status/on_ok) takes the rows-mode short path.
    FilamentMapRowsPanel* m_rows_panel{nullptr};
    wxBoxSizer*   m_rows_holder_sizer{nullptr};
    Button*       m_edit_inventory_btn{nullptr};
    Button*       m_reset_btn{nullptr};        // restores m_initial_proposal, stays open
    Button*       m_auto_apply_btn{nullptr};   // fills rows with a fresh auto-match, stays open
    CheckBox*     m_record_checkbox{nullptr};
    wxWindow*     m_record_row{nullptr};
    ThumbnailPanel* m_preview_panel{nullptr};  // current-plate preview, see update_preview()
    // Orca: construction-time COPIES of the current plate's lit / no-light renders. The
    // dialog owns its preview pixels outright: the plate's own thumbnails are invalidated while
    // this dialog is up (the queued Preview-panel switch runs inside its modal loop), and
    // re-rendering them from here is what caused the GLX BadMatch crash. Left default-constructed
    // (is_valid() == false) when no valid render existed at construction, which just means no
    // preview.
    ThumbnailData m_preview_lit;
    ThumbnailData m_preview_no_light;
    // Orca: the row proposal the dialog was constructed with (see the rows-mode ctor) -- what
    // on_reset_to_initial replays. Never mutated after construction.
    std::vector<int> m_initial_proposal;
    // Orca: whether m_initial_proposal was itself an auto-matcher
    // result (the plate had never been manually confirmed) -- the auto_matched flag
    // on_reset_to_initial's rebuild_rows_panel call replays, so Reset reproduces the same badge
    // state the dialog opened with, not just the same target picks.
    bool m_initial_proposal_was_auto{false};
    std::vector<std::string> m_filament_names;
    std::vector<int>         m_plate_filaments;
    FilamentInventory         m_inventory;
    size_t                    m_tool_count{0};
    std::string               m_printer_preset_name;
    // Orca: inventory-store routing (see FilamentInventoryStore.hpp). m_inventory above stays the
    // working copy used to build/redraw rows; m_store (plus device(), which resolves against
    // the currently selected printer preset -- same source m_printer_preset_name was built from)
    // is the source of truth for persisting edits back to the printer preset's inventory
    // (on_edit_inventory's reload, on_ok's record-to-inventory backfill).
    FilamentInventories        m_store;
    // Orca: true whenever the inventory used to build the current rows had every tool slot
    // empty; gates the "Remember these as the loaded filaments" offer on OK together with
    // AllRowsAssigned().
    bool m_inventory_was_all_unset{false};
    // Orca: armed by on_apply_automatic (the "Automatic" button),
    // disarmed by rebuild_rows_panel's default (every rebuild starts disarmed -- on_apply_
    // automatic re-arms it right after its own rebuild call) and by any later manual tile pick
    // (the wxEVT_INVALID_MANUAL_MAP handler bound in rebuild_rows_panel). Read only by get_mode();
    // see its doc for what armed vs disarmed means for OK's saved mode.
    bool m_live_auto_pending{false};

    bool m_fila_switch_ready{false};

    PageType m_page_type;

private:
    std::vector<int> m_filament_map;
    std::vector<int> m_physical_filament_map;
    std::vector<int> m_filament_volume_map;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;
    std::vector<std::string> m_filament_vendor;
    std::vector<std::string> m_filament_preset;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_FilamentMapDialog_hpp_ */
