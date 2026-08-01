#ifndef slic3r_FilamentMapDialog_hpp_
#define slic3r_FilamentMapDialog_hpp_

#include "FilamentMapPanel.hpp"
#include "libslic3r/FilamentInventory.hpp"
#include <vector>
#include <string>
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

// Orca: shows the row-mode filament-map dialog (see the rows-mode FilamentMapDialog
// constructor) for a single plate and, on OK, persists the picked map as a confirmed
// (fmmManual) mapping on that plate. Shared by try_pop_up_before_slice's per-plate loop and
// by the "Map filaments to tools..." sidebar entry point (Plater::open_filament_map_dialog_
// for_current_plate) so both flows build/apply the mapping identically.
// Returns false if the user cancelled (plate left untouched); true on OK.
bool show_filament_map_rows_dialog_for_plate(Plater* plater_ref, PartPlate* plate, const wxString& title);


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
    // filament (spec R3.3), not a bare tool; the tool-level filament_map is derived from it
    // (spec R3.5) and stays the engine/3MF-persisted routing truth.
    // filament_color/filament_type/filament_names: full project lists, 0-based by filament id.
    // filament_map: full-length 1-based stored filament->tool map, used only as the base-map
    //   passthrough for filaments not on this plate (and as a bootstrap-mode row's tool -- see
    //   FilamentMapRowsPanel).
    // physical_filament_map: full-length stored filament->physical-filament-id map (0 =
    //   unassigned). Only consulted (via compute_physical_map_proposal) when
    //   filament_map_confirmed is true; entries that still resolve to a real, non-empty
    //   physical filament are kept as the row's proposal, everything else (unset/stale/cleared)
    //   is filled from match_filaments_to_inventory. When filament_map_confirmed is false (a
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

    FilamentMapMode get_mode();
    std::vector<int> get_filament_maps() const {
        if (m_rows_panel)
            return m_filament_map;
        if (m_page_type == PageType::ptManual)
            return m_filament_map;
        return {};
    }

    // Orca: physical filament id per project filament (0 = unassigned); only meaningful in
    // rows mode (spec R3.5's second, physical-targeting map).
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

    void update_panel_status(PageType page);

    // Orca: (re)builds m_rows_panel from `proposal` (one entry per m_plate_filaments, physical
    // filament id or 0 for unassigned), wrapping it in a scrolled window when the row count is
    // large. Used both by the constructor and by on_edit_inventory() after the inventory changes.
    void rebuild_rows_panel(const std::vector<int> &proposal);
    // Seeds OK-enabled and the "Remember..." checkbox visibility from the rows panel's and
    // inventory's current state. m_rows_panel's own construction stays silent (no
    // wxEVT_INVALID_MANUAL_MAP), so callers must invoke this explicitly after (re)building it.
    void seed_rows_mode_status();

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
    CheckBox*     m_record_checkbox{nullptr};
    wxWindow*     m_record_row{nullptr};
    std::vector<std::string> m_filament_names;
    std::vector<int>         m_plate_filaments;
    FilamentInventory         m_inventory;
    size_t                    m_tool_count{0};
    std::string               m_printer_preset_name;
    // Orca: true whenever the inventory used to build the current rows had every tool slot
    // empty; gates the "Remember these as the loaded filaments" offer on OK together with
    // AllRowsAssigned().
    bool m_inventory_was_all_unset{false};

    bool m_fila_switch_ready{false};

    PageType m_page_type;

private:
    std::vector<int> m_filament_map;
    std::vector<int> m_physical_filament_map;
    std::vector<int> m_filament_volume_map;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_FilamentMapDialog_hpp_ */
