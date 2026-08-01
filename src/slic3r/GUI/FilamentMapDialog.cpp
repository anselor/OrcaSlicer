#include "FilamentMapDialog.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "CapsuleButton.hpp"
#include "MsgDialog.hpp"
#include "FilamentMapRowsPanel.hpp"
#include "FilamentInventoryStore.hpp"
#include "FilamentInventoryEditor.hpp"

#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <algorithm>

namespace Slic3r { namespace GUI {

// Orca: maximum plate-filament rows shown before FilamentMapRowsPanel is wrapped in a
// scrolled window (see wrap_rows_panel below). Keeps the dialog's height bounded regardless
// of how many filaments a plate uses, instead of the old tools-mode dialog's unbounded width
// growth with tool count.
static constexpr size_t MAX_VISIBLE_MAP_ROWS = 8;

// Orca: PartPlate::set_filament_count/on_filament_added always default a fresh filament_map
// entry to 1 (never 0/unset), so a plate that was never actually mapped is indistinguishable
// from one whose filament 1 was genuinely confirmed onto tool 1 by looking at the map array
// alone. The plate's real filament_map_mode is the primary discriminator (fmmManual is only
// ever written by a prior dialog OK or a 3mf that carried a confirmed mapping); a non-empty
// stored physical map is trusted too (spec R3.5: a plate with a confirmed physical map reuses
// it even if, e.g., the mode field round-tripped oddly through an older project). Given either
// signal, the actual proposal building (stored-id validity + matcher fallback) lives in
// libslic3r's compute_physical_map_proposal so it can be unit-tested without a wx harness.
static bool any_physical_map_set(const std::vector<int> &physical_map)
{
    return std::any_of(physical_map.begin(), physical_map.end(), [](int id) { return id > 0; });
}

// Orca: Slic3r::inventory_all_unset (libslic3r/FilamentInventory.hpp) is the single canonical
// "has this inventory ever recorded anything" check -- shared with FilamentMapRowsPanel's
// bootstrap-mode gate and compute_physical_map_proposal's bootstrap-vs-normal branch, so all
// three call sites always agree on which mode a plate is in.

static bool get_pop_up_remind_flag()
{
    auto &app_config = wxGetApp().app_config;
    return app_config->get_bool("pop_up_filament_map_dialog");
}

static void set_pop_up_remind_flag(bool remind)
{
    auto &app_config = wxGetApp().app_config;
    app_config->set_bool("pop_up_filament_map_dialog", remind);
}

static FilamentMapMode get_applied_map_mode(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_map_mode(proj_config);
    return plater_ref->get_global_filament_map_mode();
}

static std::vector<int> get_applied_map(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_maps(proj_config);
    return plater_ref->get_global_filament_map();
}

static std::vector<int> get_applied_volume_map(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_volume_maps(proj_config);
    return plater_ref->get_global_filament_volume_map();
}

extern std::string& get_left_extruder_unprintable_text();
extern std::string& get_right_extruder_unprintable_text();

// Orca: minimal smart-filament toggle. When a filament track switch is ready every AMS filament is
// reachable from both nozzles, so one filament can be assigned to multiple nozzles to maximize
// savings. The checkbox drives the enable_filament_dynamic_map project flag.
class SmartFilamentPanel : public wxPanel
{
    static constexpr int spacing = 20;

public:
    SmartFilamentPanel(wxWindow *parent) : wxPanel(parent)
    {
        SetBackgroundColour(*wxWHITE);
        wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

        main_sizer->AddSpacer(FromDIP(spacing));

        auto *separator = new wxPanel(this);
        separator->SetBackgroundColour(wxColour("#EEEEEE"));
        main_sizer->Add(separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

        main_sizer->AddSpacer(FromDIP(spacing));

        m_smart_filament_checkbox = new CheckBox(this);
        if (auto *opt = enable_filament_dynamic_map())
            m_smart_filament_checkbox->SetValue(opt->value);
        m_smart_filament_checkbox->Bind(wxEVT_TOGGLEBUTTON, &SmartFilamentPanel::on_smart_filament_checkbox, this);

        auto *label = new Label(this, _L("Enable smart filament assign: Assign one filament to multiple nozzles to maximize savings"));
        label->SetFont(Label::Body_12);

        // Orca: dropped the vendor "Learn more" tracking link (no Orca help page for this feature).

        auto *smart_sizer = new wxBoxSizer(wxHORIZONTAL);
        smart_sizer->Add(m_smart_filament_checkbox, 0, wxALIGN_CENTER_VERTICAL);
        smart_sizer->Add(label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(3));

        main_sizer->Add(smart_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));

        SetSizer(main_sizer);
        Layout();
        Fit();
        wxGetApp().UpdateDarkUIWin(this);
    }

private:
    static ConfigOptionBool *enable_filament_dynamic_map()
    {
        auto &config = wxGetApp().preset_bundle->project_config;
        return dynamic_cast<ConfigOptionBool *>(config.option("enable_filament_dynamic_map"));
    }

    void on_smart_filament_checkbox(wxCommandEvent &event)
    {
        if (auto *opt = enable_filament_dynamic_map())
            opt->value = m_smart_filament_checkbox->GetValue();
        wxGetApp().plater()->update();
        event.Skip();
    }

private:
    CheckBox *m_smart_filament_checkbox{nullptr};
};


bool show_filament_map_rows_dialog_for_plate(Plater* plater_ref, PartPlate* plate, const wxString& title)
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    std::vector<std::string> filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> filament_types  = full_config.option<ConfigOptionStrings>("filament_type")->values;
    std::vector<std::string> filament_names  = full_config.option<ConfigOptionStrings>("filament_settings_id")->values;
    const auto nozzle_diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t tool_count = nozzle_diameters->size();
    const std::string printer_preset_name = wxGetApp().preset_bundle->printers.get_selected_preset_name();

    std::vector<int> applied_maps = plate->get_real_filament_maps(wxGetApp().preset_bundle->project_config);
    applied_maps.resize(filament_colors.size(), 0);
    std::vector<int> applied_physical_maps = plate->get_real_physical_filament_maps(wxGetApp().preset_bundle->project_config);
    applied_physical_maps.resize(filament_colors.size(), 0);
    // fmmManual is only ever written by a prior dialog OK (or a 3mf that carried a confirmed
    // mapping); anything else means this plate has never actually been through this dialog, so
    // applied_maps' all-1-by-default entries must not be trusted as real picks. A non-empty
    // physical map is trusted the same way even if the mode signal didn't carry over (see the
    // comment above any_physical_map_set).
    const bool filament_map_confirmed =
        plate->get_real_filament_map_mode(wxGetApp().preset_bundle->project_config) == fmmManual ||
        any_physical_map_set(applied_physical_maps);

    FilamentInventory inventory = load_filament_inventory(printer_preset_name, tool_count);
    std::vector<int> plate_filaments = plate->get_extruders(true); // 1-based used filaments

    FilamentMapDialog map_dlg(plater_ref, filament_colors, filament_types, filament_names,
                              applied_maps, applied_physical_maps, plate_filaments, inventory, tool_count,
                              printer_preset_name, filament_map_confirmed, title);
    if (map_dlg.ShowModal() != wxID_OK)
        return false;
    // Orca: set_filament_map_mode MUST run before the two maps below, not after -- when the
    // mode actually changes it calls PartPlate::clear_filament_map() (erases "filament_map"),
    // so setting the tool map first and the mode second would silently wipe the map right back
    // out. It does not touch "filament_physical_map", so that ordering is inert between the two
    // set_*_maps calls; physical is set first only to match the brief's listed order (spec R3.5:
    // tool map stays the engine/3MF routing truth, derived from physical ownership).
    plate->set_filament_map_mode(fmmManual);
    plate->set_physical_filament_maps(map_dlg.get_physical_filament_maps());
    plate->set_filament_maps(map_dlg.get_filament_maps());
    return true;
}

bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up)
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    const auto nozzle_diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameters->size() <= 1)
        return true;

    // Opted-in non-BBL multi-tool printers: per-plate manual mapping, asked on every slice.
    if (filament_mapping_enabled(full_config)) {
        std::vector<PartPlate*> plates;
        if (is_slice_all) {
            for (PartPlate* plate : plater_ref->get_partplate_list().get_plate_list())
                if (plate != nullptr && !plate->empty())
                    plates.push_back(plate);
        } else {
            plates.push_back(partplate_ref);
        }

        for (size_t i = 0; i < plates.size(); ++i) {
            PartPlate* plate = plates[i];
            if (plate->get_extruders(true).empty())   // 1-based used filaments
                continue;

            wxString title = is_slice_all
                ? wxString::Format(_L("Map filaments to tools — Plate %d of %d"), (int) i + 1, (int) plates.size())
                : _L("Map filaments to tools");
            if (!show_filament_map_rows_dialog_for_plate(plater_ref, plate, title))
                return false;   // cancel aborts the slice (and the batch)
        }
        plater_ref->update();
        return true;
    }

    // The filament-grouping dialog is specifically designed for BBL dual-nozzle printers
    // (e.g. H2D) where filaments must be assigned to a left or right nozzle.
    // For toolchangers (≥3 tools) and all non-BBL printers the dialog is irrelevant and
    // confusing; skip it entirely so slicing proceeds without interruption. (#12390)
    PresetBundle* preset = wxGetApp().preset_bundle;
    if (!preset || !preset->is_bbl_vendor() || nozzle_diameters->size() != 2)
        return true;

    bool sync_plate = true;

    std::vector<std::string> filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> filament_types = full_config.option<ConfigOptionStrings>("filament_type")->values;
    FilamentMapMode applied_mode = get_applied_map_mode(full_config, plater_ref,partplate_ref, sync_plate);
    std::vector<int> applied_maps = get_applied_map(full_config, plater_ref, partplate_ref, sync_plate);
    std::vector<int> applied_volume_maps = get_applied_volume_map(full_config, plater_ref, partplate_ref, sync_plate);
    applied_maps.resize(filament_colors.size(), 1);
    applied_volume_maps.resize(filament_colors.size(), 0);

    if (!force_pop_up && applied_mode != fmmManual)
        return true;

    std::vector<int> filament_lists;
    if (is_slice_all) {
        filament_lists.resize(filament_colors.size());
        std::iota(filament_lists.begin(), filament_lists.end(), 1);
    }
    else {
        filament_lists = partplate_ref->get_extruders();
    }

    FilamentMapDialog map_dlg(plater_ref,
        filament_colors,
        filament_types,
        applied_maps,
        applied_volume_maps,
        filament_lists,
        applied_mode,
        plater_ref->get_machine_sync_status(),
        false,
        false
    );
    auto ret = map_dlg.ShowModal();

    if (ret == wxID_OK) {
        FilamentMapMode new_mode = map_dlg.get_mode();
        std::vector<int> new_maps = map_dlg.get_filament_maps();
        std::vector<int> new_volume_maps = map_dlg.get_filament_volume_maps();
        if (sync_plate) {
            if (is_slice_all) {
                auto plate_list = plater_ref->get_partplate_list().get_plate_list();
                for (int i = 0; i < plate_list.size(); ++i) {
                    plate_list[i]->set_filament_map_mode(new_mode);
                    if (new_mode == fmmManual) {
                        plate_list[i]->set_filament_maps(new_maps);
                        plate_list[i]->set_filament_volume_maps(new_volume_maps);
                    }
                }
            }
            else {
                partplate_ref->set_filament_map_mode(new_mode);
                if (new_mode == fmmManual) {
                    partplate_ref->set_filament_maps(new_maps);
                    partplate_ref->set_filament_volume_maps(new_volume_maps);
                }
            }
        }
        else {
            plater_ref->set_global_filament_map_mode(new_mode);
            if (new_mode == fmmManual) {
                plater_ref->set_global_filament_map(new_maps);
                plater_ref->set_global_filament_volume_map(new_volume_maps);
            }
        }
        plater_ref->update();
        // check whether able to slice, if not, return false
        if (!get_left_extruder_unprintable_text().empty() || !get_right_extruder_unprintable_text().empty()){
            return false;
        }
        return true;
    }
    return false;
}

FilamentMapDialog::FilamentMapDialog(wxWindow                       *parent,
                                     const std::vector<std::string> &filament_color,
                                     const std::vector<std::string> &filament_type,
                                     const std::vector<int>         &filament_map,
                                     const std::vector<int>         &filament_volume_map,
                                     const std::vector<int>         &filaments,
                                     const FilamentMapMode           mode,
                                     bool                            machine_synced,
                                     bool                            show_default,
                                     bool                            with_checkbox)
    : wxDialog(parent, wxID_ANY, _L("Filament grouping"), wxDefaultPosition, wxDefaultSize,wxDEFAULT_DIALOG_STYLE)
    , m_filament_color(filament_color)
    , m_filament_type(filament_type)
    , m_filament_map(filament_map)
    , m_filament_volume_map(filament_volume_map)
{
    SetBackgroundColour(*wxWHITE);

    SetMinSize(wxSize(FromDIP(580), -1));
    SetMaxSize(wxSize(FromDIP(580), -1));

    // Orca: when a filament track switch is ready, every AMS filament reaches both nozzles, so the
    // Match/Convenience sub-mode is dropped and Auto is presented purely as a filament-saving mode.
    // Orca has no fmmAutoForQuality, so "only saving" collapses to "switch ready" and the remaining
    // auto mode set is { fmmAutoForFlush }.
    m_fila_switch_ready              = wxGetApp().sidebar().is_fila_switch_ready();
    const bool only_saving_mode     = m_fila_switch_ready;
    const bool auto_match_available = machine_synced && !m_fila_switch_ready;

    if (mode < fmmManual)
        m_page_type = PageType::ptAuto;
    else if (mode == fmmManual || mode == fmmNozzleManual)
        // Orca: there is no dedicated nozzle-manual page, so treat an fmmNozzleManual plate as a manual variant and show the Custom page instead of misfiling it as "Same as Global".
        m_page_type = PageType::ptManual;
    else
        m_page_type = PageType::ptDefault;

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(22));

    wxBoxSizer *mode_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_auto_btn   = new CapsuleButton(this, PageType::ptAuto, only_saving_mode ? _L("Fila Saving") : _L("Auto"), false);
    m_manual_btn = new CapsuleButton(this, PageType::ptManual, _L("Custom"), false);
    if (show_default)
        m_default_btn = new CapsuleButton(this, PageType::ptDefault, _L("Same as Global"), true);
    else
        m_default_btn = nullptr;

    const int button_padding = FromDIP(2);
    mode_sizer->AddStretchSpacer();
    mode_sizer->Add(m_auto_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    mode_sizer->Add(m_manual_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    if (show_default) mode_sizer->Add(m_default_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    mode_sizer->AddStretchSpacer();

    main_sizer->Add(mode_sizer, 0, wxEXPAND);
    main_sizer->AddSpacer(FromDIP(24));

    auto            panel_sizer       = new wxBoxSizer(wxHORIZONTAL);

    // Orca: fall back to the saving mode whenever Match is unavailable, which now also covers the
    // filament-track-switch-ready case (auto_match_available folds in !m_fila_switch_ready).
    FilamentMapMode default_auto_mode = mode >= fmmManual ? fmmAutoForFlush :
        mode == fmmAutoForMatch && !auto_match_available ? fmmAutoForFlush :
        mode;

    m_manual_map_panel                = new FilamentMapManualPanel(this, m_filament_color, m_filament_type, filaments, filament_map, filament_volume_map);
    // A manual grouping can point filaments at a nozzle volume the extruder does not physically
    // carry; the panel's validation timer reports that here so OK is gated on a printable map.
    m_manual_map_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        if (m_page_type != PageType::ptManual) {
            if (!m_ok_btn->IsEnabled()) { m_ok_btn->Enable(); }
            return;
        }
        if (event.GetInt()) {
            if (!m_ok_btn->IsEnabled()) { m_ok_btn->Enable(); }
        } else {
            if (m_ok_btn->IsEnabled()) { m_ok_btn->Disable(); }
        }
    });
    m_auto_map_panel                  = new FilamentMapAutoPanel(this, default_auto_mode, auto_match_available);
    if (show_default)
        m_default_map_panel = new FilamentMapDefaultPanel(this);
    else
        m_default_map_panel = nullptr;

    panel_sizer->Add(m_manual_map_panel, 0, wxEXPAND);
    panel_sizer->Add(m_auto_map_panel, 0, wxEXPAND);
    if (show_default) panel_sizer->Add(m_default_map_panel, 0, wxEXPAND);
    main_sizer->Add(panel_sizer, 0, wxEXPAND);

    // Smart filament section, shown only in filament-saving (flush) mode when the switch is ready.
    if (m_fila_switch_ready) {
        m_smart_filament = new SmartFilamentPanel(this);
        m_smart_filament->Show(get_mode() == fmmAutoForFlush);
        main_sizer->Add(m_smart_filament, 0, wxEXPAND);
    }

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->Fit(bottom_panel);

    if(with_checkbox)
    {
        auto* checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_checkbox = new CheckBox(bottom_panel);
        m_checkbox->Bind(wxEVT_TOGGLEBUTTON, &FilamentMapDialog::on_checkbox, this);
        checkbox_sizer->Add(m_checkbox, 0, wxALIGN_CENTER, 0);

        auto* checkbox_label = new Label(bottom_panel, _L("Don't remind me again"));
        checkbox_label->SetFont(Label::Body_12);
        checkbox_sizer->Add(checkbox_label, 0, wxLEFT| wxALIGN_CENTER , FromDIP(3));

        bottom_sizer->Add(checkbox_sizer, 0 ,  wxALIGN_CENTER | wxALL, FromDIP(15));
    }

    bottom_sizer->AddStretchSpacer();

    {
        auto dlg_btns = new DialogButtons(bottom_panel, {"OK", "Cancel"});
        m_ok_btn      = dlg_btns->GetOK();
        m_cancel_btn  = dlg_btns->GetCANCEL();

        bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    }
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    m_ok_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_ok, this);
    m_cancel_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_cancel, this);
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (IsModal())
                EndModal(wxID_CANCEL);
            else
                Close();
            return;
        }
        e.Skip();
    });

    m_auto_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);
    m_manual_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);
    if (show_default) m_default_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);

    SetSizer(main_sizer);
    Layout();
    Fit();

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

FilamentMapDialog::FilamentMapDialog(wxWindow                       *parent,
                                     const std::vector<std::string> &filament_color,
                                     const std::vector<std::string> &filament_type,
                                     const std::vector<std::string> &filament_names,
                                     const std::vector<int>         &filament_map,
                                     const std::vector<int>         &physical_filament_map,
                                     const std::vector<int>         &plate_filaments,
                                     const FilamentInventory         &inventory,
                                     size_t                           tool_count,
                                     const std::string               &printer_preset_name,
                                     bool                             filament_map_confirmed,
                                     const wxString                  &title)
    : wxDialog(parent, wxID_ANY, _L("Map filaments to tools"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_filament_names(filament_names)
    , m_plate_filaments(plate_filaments)
    , m_inventory(inventory)
    , m_tool_count(tool_count)
    , m_printer_preset_name(printer_preset_name)
    , m_page_type(PageType::ptManual)
    , m_filament_map(filament_map)
    , m_physical_filament_map(physical_filament_map)
    , m_filament_color(filament_color)
    , m_filament_type(filament_type)
{
    SetBackgroundColour(*wxWHITE);
    SetTitle(title);

    // Orca: rows mode is a single vertical column, so unlike the old tools panel (one ~260 DIP
    // column per physical tool, needing the dialog to grow with tool_count), width stays fixed;
    // only height grows with plate filament count, capped via wrap_rows_panel's scroll wrapper.
    SetMinSize(wxSize(FromDIP(560), -1));
    SetMaxSize(wxSize(FromDIP(560), -1));

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(22));

    auto *title_label = new Label(this, title);
    title_label->SetFont(Label::Head_18);
    main_sizer->Add(title_label, 0, wxALIGN_CENTER);
    main_sizer->AddSpacer(FromDIP(20));

    m_rows_holder_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_rows_holder_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    m_inventory_was_all_unset = inventory_all_unset(m_inventory);
    std::vector<int> proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_plate_filaments,
                                                               m_physical_filament_map, m_filament_map, m_inventory,
                                                               filament_map_confirmed);
    rebuild_rows_panel(proposal);

    main_sizer->AddSpacer(FromDIP(8));
    m_edit_inventory_btn = new Button(this, _L("Edit physical filaments..."));
    m_edit_inventory_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_edit_inventory_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_edit_inventory, this);
    main_sizer->Add(m_edit_inventory_btn, 0, wxLEFT | wxRIGHT, FromDIP(15));

    // Orca: offer to persist the current picks as the printer's loaded-filament inventory when
    // the inventory used to build the proposal was empty; visibility re-evaluated alongside OK
    // gating in seed_rows_mode_status() since it depends on the (changing) row state too.
    {
        wxPanel *record_panel = new wxPanel(this);
        record_panel->SetBackgroundColour(*wxWHITE);
        auto *record_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_record_checkbox = new CheckBox(record_panel);
        m_record_checkbox->SetValue(true);
        record_sizer->Add(m_record_checkbox, 0, wxALIGN_CENTER_VERTICAL);
        auto *record_label = new Label(record_panel, _L("Remember these as the loaded filaments"));
        record_label->SetFont(Label::Body_12);
        record_sizer->Add(record_label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
        record_panel->SetSizer(record_sizer);
        m_record_row = record_panel;
        main_sizer->AddSpacer(FromDIP(8));
        main_sizer->Add(m_record_row, 0, wxLEFT | wxRIGHT, FromDIP(15));
    }

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->Fit(bottom_panel);

    bottom_sizer->AddStretchSpacer();

    {
        auto dlg_btns = new DialogButtons(bottom_panel, {"OK", "Cancel"});
        m_ok_btn      = dlg_btns->GetOK();
        m_cancel_btn  = dlg_btns->GetCANCEL();

        bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    }
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    // m_rows_panel's own construction stays silent (no wxEVT_INVALID_MANUAL_MAP -- see its
    // header), so the initial OK/record-checkbox state is seeded explicitly now that m_ok_btn
    // and m_record_row both exist.
    seed_rows_mode_status();

    m_ok_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_ok, this);
    m_cancel_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_cancel, this);
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (IsModal())
                EndModal(wxID_CANCEL);
            else
                Close();
            return;
        }
        e.Skip();
    });

    SetSizer(main_sizer);
    Layout();
    Fit();

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentMapDialog::rebuild_rows_panel(const std::vector<int> &proposal)
{
    // Clear(true) destroys the previous panel (and any scroll wrapper) along with its sizer
    // item; m_rows_panel itself is about to be reassigned below.
    m_rows_holder_sizer->Clear(true);

    m_rows_panel = new FilamentMapRowsPanel(this, m_filament_color, m_filament_type, m_filament_names,
                                            m_plate_filaments, proposal, m_inventory, m_tool_count);
    m_rows_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        seed_rows_mode_status();
        event.Skip();
    });

    wxWindow *rows_widget = m_rows_panel;
    if (m_plate_filaments.size() > MAX_VISIBLE_MAP_ROWS) {
        // Orca: follows FilamentPickerDialog::CreateColorGrid's scrolled-window pattern -- cap
        // the visible height to MAX_VISIBLE_MAP_ROWS rows (derived from the panel's own
        // best-size, since row height isn't a fixed constant here) and let the rest scroll.
        wxSize best = m_rows_panel->GetBestSize();
        int    per_row = m_plate_filaments.size() ? std::max(1, best.GetHeight() / (int) m_plate_filaments.size()) : FromDIP(36);

        auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxNO_BORDER);
        m_rows_panel->Reparent(scroll);
        auto *scroll_sizer = new wxBoxSizer(wxVERTICAL);
        scroll_sizer->Add(m_rows_panel, 1, wxEXPAND);
        scroll->SetSizer(scroll_sizer);
        scroll->SetMinSize(wxSize(best.GetWidth() + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X), per_row * (int) MAX_VISIBLE_MAP_ROWS));
        scroll->FitInside();
        scroll->SetScrollRate(0, per_row);
        rows_widget = scroll;
    }

    m_rows_holder_sizer->Add(rows_widget, 0, wxEXPAND);

    Layout();
    Fit();
}

void FilamentMapDialog::seed_rows_mode_status()
{
    if (!m_rows_panel) return;

    bool all_assigned = m_rows_panel->AllRowsAssigned();
    if (m_ok_btn)
        m_ok_btn->Enable(all_assigned);

    if (m_record_row)
        m_record_row->Show(m_inventory_was_all_unset && all_assigned);

    Layout();
}

void FilamentMapDialog::on_edit_inventory(wxCommandEvent &)
{
    // Probe the panel's current per-row selections before the edit, in both spaces: zero out
    // every plate-filament slot in the base physical/tool maps first, then read
    // GetPhysicalMaps()/GetFilamentMaps() -- a row the user actually picked reports its real
    // (>0 physical id, or >=1 tool) value; a still-unassigned row falls through to the probe's
    // 0 in both. The tool-space probe matters when the panel was in bootstrap mode (inventory
    // had nothing recorded, so every pick is a bare tool with no physical id at all -- see
    // FilamentMapRowsPanel::BuildTargetOptions): without it, an edit that adds the printer's
    // first physical filament would lose whatever the user had already picked. Both probes are
    // overwritten by the real SetBase(Physical)Map calls in on_ok() regardless of what happens
    // here.
    std::vector<int> probe_physical(m_filament_color.size(), 0);
    std::vector<int> probe_tool(m_filament_color.size(), 0);
    m_rows_panel->SetBasePhysicalMap(probe_physical);
    m_rows_panel->SetBaseMap(probe_tool);
    std::vector<int> current_physical = m_rows_panel->GetPhysicalMaps();
    std::vector<int> current_tool     = m_rows_panel->GetFilamentMaps();

    FilamentInventoryEditor editor(this, m_printer_preset_name, m_tool_count);
    if (editor.ShowModal() != wxID_OK)
        return;

    m_inventory               = load_filament_inventory(m_printer_preset_name, m_tool_count);
    m_inventory_was_all_unset = inventory_all_unset(m_inventory);

    // Treat the user's current picks as a confirmed stored map for re-proposing: a picked row
    // keeps its physical filament id (or, in bootstrap mode, its tool) when it still resolves
    // to something real after the edit; anything else (unassigned, or a slot the edit just
    // cleared/removed) falls back to auto-match, exactly like compute_physical_map_proposal's
    // stale-id case.
    std::vector<int> proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_plate_filaments,
                                                               current_physical, current_tool, m_inventory,
                                                               /*stored_map_confirmed=*/true);

    rebuild_rows_panel(proposal);
    seed_rows_mode_status();
}

FilamentMapMode FilamentMapDialog::get_mode()
{
    if (m_rows_panel) return fmmManual;
    if (m_page_type == PageType::ptAuto) return m_auto_map_panel->GetMode();
    if (m_page_type == PageType::ptManual) return fmmManual;
    return fmmDefault;
}

int FilamentMapDialog::ShowModal()
{
    if (!m_rows_panel)
        update_panel_status(m_page_type);
    return wxDialog::ShowModal();
}

void FilamentMapDialog::on_checkbox(wxCommandEvent &event)
{
    bool is_checked = m_checkbox->GetValue();
    m_checkbox->SetValue(is_checked);
    set_pop_up_remind_flag(!is_checked);

    if (is_checked) {
        MessageDialog dialog(nullptr, _L("No further pop-up will appear. You can reopen it in 'Preferences'"), _L("Tips"), wxICON_INFORMATION | wxOK);
        dialog.ShowModal();
        this->Close();
    }

    event.Skip();
}

void FilamentMapDialog::on_ok(wxCommandEvent &event)
{
    if (m_rows_panel) {
        m_rows_panel->SetBaseMap(m_filament_map);
        m_rows_panel->SetBasePhysicalMap(m_physical_filament_map);
        m_physical_filament_map = m_rows_panel->GetPhysicalMaps();
        m_filament_map          = m_rows_panel->GetFilamentMaps();

        // Orca: only offer to persist picks when the inventory used to build this dialog's
        // proposal was entirely empty and every row ended up with a manual pick -- otherwise
        // the checkbox is hidden (seed_rows_mode_status) and stays unchecked-equivalent. This
        // is also exactly the bootstrap-mode case (FilamentMapRowsPanel::BuildTargetOptions):
        // every plate row's physical id is still 0 at this point, so the loop below backfills
        // m_physical_filament_map from the ids this write mints.
        if (m_record_row && m_record_row->IsShown() && m_record_checkbox && m_record_checkbox->GetValue()) {
            FilamentInventory new_inv;
            new_inv.tools.resize(m_tool_count);
            for (auto &slots : new_inv.tools)
                slots.resize(1); // slot 0 (loaded) must always be present
            for (int f : m_plate_filaments) {
                if (f < 1 || f > (int) m_filament_map.size()) continue;
                int tool = m_filament_map[f - 1];
                if (tool < 1 || tool > (int) m_tool_count) continue;
                LoadedFilament &slot = new_inv.tools[tool - 1][0];
                slot.color = (f - 1 < (int) m_filament_color.size()) ? m_filament_color[f - 1] : std::string();
                slot.type  = (f - 1 < (int) m_filament_type.size()) ? m_filament_type[f - 1] : std::string();
            }
            // Orca: this path only ever writes the loaded slot; a stable id is assigned here
            // since a swappable slot can only be added later via the Physical Filaments editor.
            // Seed from the loaded inventory's next_id, not 1, so a re-mint doesn't reissue ids
            // already claimed by physical filaments recorded elsewhere (other plates/slots).
            int next_id = m_inventory.next_id > 0 ? m_inventory.next_id : 1;
            for (auto &slots : new_inv.tools)
                if (!slots[0].empty())
                    slots[0].id = next_id++;
            new_inv.next_id = next_id;
            save_filament_inventory(m_printer_preset_name, new_inv);

            // Close the loop for bootstrap rows: point this plate's physical map at the ids we
            // just minted for the tools the user picked, so a later dialog open sees a
            // confirmed physical map instead of re-triggering bootstrap.
            for (int f : m_plate_filaments) {
                if (f < 1 || f > (int) m_physical_filament_map.size()) continue;
                if (m_physical_filament_map[f - 1] > 0) continue; // already a real target
                int tool = (f <= (int) m_filament_map.size()) ? m_filament_map[f - 1] : 0;
                if (tool < 1 || tool > (int) m_tool_count) continue;
                m_physical_filament_map[f - 1] = new_inv.tools[tool - 1][0].id;
            }
        }
    } else if (m_page_type == PageType::ptManual) {
        m_filament_map        = m_manual_map_panel->GetFilamentMaps();
        m_filament_volume_map = m_manual_map_panel->GetFilamentVolumeMaps();
    }

    EndModal(wxID_OK);
}

void FilamentMapDialog::on_cancel(wxCommandEvent &event) { EndModal(wxID_CANCEL); }

void FilamentMapDialog::update_panel_status(PageType page)
{
    // Orca: only the mode-switching constructor wires on_switch_mode to this; the rows-mode
    // dialog has no mode buttons, so this must never run against its (null) panel members.
    if (m_rows_panel)
        return;

    std::vector<CapsuleButton*>button_list = { m_default_btn,m_manual_btn,m_auto_btn };
    for (auto p : button_list) {
        if (p && p->IsSelected()) {
            p->Select(false);
        }
    }
    std::vector<wxPanel*>panel_list = { m_default_map_panel,m_manual_map_panel,m_auto_map_panel };
    for (auto p : panel_list) {
        if (p && p->IsShown()) {
            p->Hide();
        }
    }

    if (page == PageType::ptDefault) {
        if (m_default_btn && m_default_map_panel) {
            m_default_btn->Select(true);
            m_default_map_panel->Show();
        }
    }
    if (page == PageType::ptManual) {
        m_manual_btn->Select(true);
        m_manual_map_panel->Show();
    }
    if (page == PageType::ptAuto) {
        m_auto_btn->Select(true);
        m_auto_map_panel->Show();
    }

    // The nozzle-availability gate only constrains manual grouping; every other page must
    // leave OK usable even if the manual page had disabled it.
    if (page != PageType::ptManual && m_ok_btn && !m_ok_btn->IsEnabled())
        m_ok_btn->Enable();

    if (m_smart_filament)
        m_smart_filament->Show(get_mode() == fmmAutoForFlush);

    Layout();
    Fit();
}

void FilamentMapDialog::on_switch_mode(wxCommandEvent &event)
{
    int win_id  = event.GetId();
    m_page_type = PageType(win_id);

    update_panel_status(m_page_type);
    event.Skip();
}

void FilamentMapDialog::set_modal_btn_labels(const wxString &ok_label, const wxString &cancel_label)
{
    m_ok_btn->SetLabel(ok_label);
    m_cancel_btn->SetLabel(cancel_label);
}

}} // namespace Slic3r::GUI
