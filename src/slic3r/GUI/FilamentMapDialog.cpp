#include "FilamentMapDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "CapsuleButton.hpp"
#include "MsgDialog.hpp"
#include "FilamentMapRowsPanel.hpp"
#include "ActivePrinterSession.hpp"
#include "FilamentInventoryStore.hpp"
#include "FilamentInventoryEditor.hpp"
#include "SelectMachine.hpp" // ThumbnailPanel (REUSE-AS-IS), see update_preview()
#include "3DScene.hpp"       // adjust_color_for_rendering
#include "GLCanvas3D.hpp"    // renderability probe, see refresh_plate_thumbnails_for_preview

#include <wx/glcanvas.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/image.h>
#include <algorithm>
#include <unordered_map>

namespace Slic3r { namespace GUI {

// Orca: maximum plate-filament rows shown before FilamentMapRowsPanel is wrapped in a
// scrolled window (see wrap_rows_panel below). Keeps the dialog's height bounded regardless
// of how many filaments a plate uses.
static constexpr size_t MAX_VISIBLE_MAP_ROWS = 8;

// Height of one wrapped tile row in FilamentMapRowsPanel's tiles_sizer -- the
// tile itself is a fixed FromDIP(50) (MaterialItem::messure_size with no mapped-nozzle string,
// which is BadgedSyncItem's case here), plus the FromDIP(5) top/bottom padding tiles_sizer adds
// around every tile (wxALL). Used below to size the scroll wrapper to a whole number of tile
// rows -- NOT GetBestSize().GetHeight() / filament_count, which assumed one row per filament and
// silently collapsed once tiles started wrapping horizontally.
static constexpr int TILE_ROW_HEIGHT_DIP = 60;

// Orca: PartPlate::set_filament_count/on_filament_added always default a fresh filament_map
// entry to 1 (never 0/unset), so a plate that was never actually mapped is indistinguishable
// from one whose filament 1 was genuinely confirmed onto tool 1 by looking at the map array
// alone. The plate's real filament_map_mode is the primary discriminator (fmmManual is only
// ever written by a prior dialog OK or a 3mf that carried a confirmed mapping); a non-empty
// stored physical map is trusted too (a plate with a confirmed physical map reuses
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


// Orca: refresh the cached plate thumbnails the mapping dialog's preview snapshots, from
// the CALLER's normal event handler -- never from the dialog itself. update_all_plate_thumbnails
// drives GLCanvas3D::render_thumbnail, which makes the 3D canvas's GL context current; doing that
// from inside a modal dialog's construction (as FilamentMapDialog::update_preview used to) can
// bind the context to a drawable a queued panel switch has already unmapped, which X rejects with
// BadMatch (GLX request 148, minor 11) and which kills the process on WSLg. This mirrors how the
// sync dialog gets its preview data (Plater.cpp, update_all_plate_thumbnails immediately before
// constructing SyncAmsInfoDialog).
//
// The canvas is probed first: if it is not initialized or not on screen -- e.g. the slice was
// triggered while the Preview panel is up, so the 3D canvas is hidden -- nothing is rendered and
// the dialog simply shows no preview. A missing preview is never worth an X error.
static void refresh_plate_thumbnails_for_preview(Plater *plater_ref)
{
    if (!plater_ref)
        return;
    GLCanvas3D *canvas = plater_ref->get_view3D_canvas3D();
    if (!canvas || !canvas->is_initialized())
        return;
    const wxGLCanvas *gl_canvas = canvas->get_wxglcanvas();
    if (!gl_canvas || !gl_canvas->IsShownOnScreen())
        return;
    plater_ref->update_all_plate_thumbnails(false);
}

namespace {

// Orca: inputs shared by every "show the rows-mode mapping dialog" entry point -- the project's
// filament lists, the active printer's identity, and its tool inventory. Never touches a
// PartPlate: plate-derived inputs (applied maps, the filament_map_confirmed flag, the plate's
// used-filament list) are gathered separately by each caller, since a send-time caller has no
// plate to read them from.
struct RowsDialogInputs
{
    std::vector<std::string> filament_colors;
    std::vector<std::string> filament_types;
    std::vector<std::string> filament_names;
    // The same profile the tool count and inventory below resolve against -- the inventory this
    // dialog edits is keyed by this name, so it must not come from a different resolution.
    std::string          printer_preset_name;
    FilamentInventories   store;
    // Tools a filament can be mapped to: physical nozzles, or, when the device addresses more
    // logical tools than physical nozzles, those logical tools (tool numbers run past the
    // physical tool count).
    size_t                tool_count{0};
    FilamentInventory     inventory;
};

RowsDialogInputs gather_rows_dialog_inputs()
{
    RowsDialogInputs in;
    auto full_config    = wxGetApp().preset_bundle->full_config();
    in.filament_colors  = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    in.filament_types   = full_config.option<ConfigOptionStrings>("filament_type")->values;
    in.filament_names   = full_config.option<ConfigOptionStrings>("filament_settings_id")->values;
    in.printer_preset_name = active_printer_session().profile().name;

    in.tool_count = resolve_active_printer_tool_count(in.store);
    const Preset &printer = active_printer_session().profile();
    in.inventory  = current_inventory_for_preset(printer, in.store, in.tool_count);
    return in;
}

} // namespace

std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title)
{
    RowsDialogInputs in = gather_rows_dialog_inputs();

    // Orca: no plate to read a stored map from at send time -- pass full-length all-zero base
    // maps together with filament_map_confirmed=false ("first-ever visit" semantics): the
    // rows-mode ctor ignores these entirely (compute_physical_map_proposal only consults them
    // when filament_map_confirmed is true) and proposes every row straight from the inventory
    // auto-match instead. See the rows-mode ctor's filament_map/physical_filament_map doc in
    // the header.
    std::vector<int> base_map(in.filament_colors.size(), 0);
    std::vector<int> base_physical_map(in.filament_colors.size(), 0);

    // Orca: intentionally skips refresh_plate_thumbnails_for_preview and any plate thumbnail
    // lookup -- both are plate/GL-coupled (see refresh_plate_thumbnails_for_preview's doc). The
    // rows-mode ctor's own preview snapshot only fires when wxGetApp().plater() has a current
    // plate with valid cached renders (ThumbnailData::is_valid()); it copies pixels, it never
    // renders, so leaving that lookup as-is here still does no GL work -- absent or missing
    // thumbnails just leave the preview panel hidden, same as any other "no snapshot" case.
    FilamentMapDialog map_dlg(parent, in.filament_colors, in.filament_types, in.filament_names,
                              base_map, base_physical_map, plate_filaments, in.inventory, in.tool_count,
                              in.printer_preset_name, /*filament_map_confirmed=*/false, title);
    if (map_dlg.ShowModal() != wxID_OK)
        return std::nullopt;

    // Orca: read the map regardless of get_mode() -- the Automatic/live-mode arming
    // (m_live_auto_pending) only changes what a caller persists onto a plate's
    // filament_map_mode, which this caller has none of; OK here is always a plain frozen pick.
    return map_dlg.get_filament_maps();
}

bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up)
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    // Deliberately the PHYSICAL nozzle count, not addressable_tool_count_of: this is the "does
    // the printer have more than one tool at all" gate. Resolving the current device
    // here just to reach the same answer would load the registry on every slice, including
    // single-extruder printers.
    const auto nozzle_diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameters->size() <= 1)
        return true;

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
    m_filament_preset = wxGetApp().preset_bundle->filament_presets;
    // Orca: needed so this dialog's proposal uses ProjectFilamentInfo::vendor as a
    // tie-break -- see build_project_filament_info's shared builder.
    // PresetBundle::full_config() returns BY VALUE: the result must be held in a named variable
    // before any option pointer into it is dereferenced. Reading `...full_config().option<..>(k)`
    // inside an if-condition and using the pointer in the body dereferences freed storage -- the
    // temporary dies at the end of the condition's full-expression, so the vendor vector was
    // copied out of a destroyed config and threw std::length_error/std::bad_alloc on a garbage
    // string length.
    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    if (const auto *vendors = full_config.option<ConfigOptionStrings>("filament_vendor"))
        m_filament_vendor = vendors->values;

    SetBackgroundColour(*wxWHITE);
    SetTitle(title);

    // Orca: rows mode is a single vertical column; height grows with plate filament count
    // (capped via the scroll wrapper). Width is content-driven: the row combos size themselves
    // to their widest option label (FilamentMapRowsPanel), so no max cap here -- a cap would
    // clip long preset-bearing labels (Fit() can never grow past a SetMaxSize).
    SetMinSize(wxSize(FromDIP(560), -1));

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    // Orca: no in-client-area title label here -- SetTitle(title) above already puts it in the
    // native titlebar; an inner Label repeated the same text as a second, larger heading.
    main_sizer->AddSpacer(FromDIP(20));

    m_rows_holder_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_rows_holder_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    m_inventory_was_all_unset = inventory_all_unset(m_inventory);
    std::vector<int> proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor, m_filament_preset, m_plate_filaments,
                                                               m_physical_filament_map, m_filament_map, m_inventory,
                                                               filament_map_confirmed);
    // Orca: snapshot of what the dialog is about to show, for on_reset_to_initial to replay.
    m_initial_proposal = proposal;
    // An unconfirmed plate's initial proposal is entirely auto-matcher
    // output (compute_physical_map_proposal ignores the stored maps when !filament_map_confirmed)
    // -- every row this seeds starts with its auto-matched badge shown.
    m_initial_proposal_was_auto = !filament_map_confirmed;
    rebuild_rows_panel(proposal, m_initial_proposal_was_auto);

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

    // Orca: snapshot the current plate's cached renders ONCE, here, and recolor from the
    // snapshot from then on. The dialog must never trigger a render of its own (see
    // refresh_plate_thumbnails_for_preview, which the caller runs before constructing us), and the
    // plate's own thumbnails are invalidated out from under a live dialog anyway -- the queued
    // switch to the Preview panel calls invalid_all_plate_thumbnails() and is dispatched inside
    // this dialog's modal loop. Copying decouples the preview from that lifetime entirely: every
    // later update_preview() (a tile pick, Reset, Automatic, an inventory edit) recolors these
    // same pixels instead of reaching back into the plate.
    if (Plater *preview_plater = wxGetApp().plater()) {
        if (PartPlate *curr_plate = preview_plater->get_partplate_list().get_curr_plate()) {
            if (curr_plate->thumbnail_data.is_valid() && curr_plate->no_light_thumbnail_data.is_valid()) {
                m_preview_lit      = curr_plate->thumbnail_data;
                m_preview_no_light = curr_plate->no_light_thumbnail_data;
            }
        }
    }

    // Orca: current-plate preview, re-colored by the rows' current picks -- below the
    // tile grid rather than beside it, to keep the dialog's width driven only by the rows panel
    // (see the ctor's SetMinSize comment above). update_preview() (called from
    // seed_rows_mode_status) fills it in; content is set once real thumbnail data exists, so it
    // starts hidden rather than showing a blank square.
    main_sizer->AddSpacer(FromDIP(8));
    m_preview_panel = new ThumbnailPanel(this);
    m_preview_panel->SetMinSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->SetMaxSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->Hide();
    main_sizer->Add(m_preview_panel, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(15));

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->Fit(bottom_panel);

    // Orca: "Reset" reverts the rows to what the dialog was opened
    // with -- it neither touches the plate nor closes the dialog (was: "Reset to automatic",
    // which set the plate to fmmDefault and closed; see on_apply_automatic's doc for what's left
    // of that capability).
    m_reset_btn = new Button(bottom_panel, _L("Reset"));
    m_reset_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_reset_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_reset_to_initial, this);
    bottom_sizer->Add(m_reset_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(15));

    m_auto_apply_btn = new Button(bottom_panel, _L("Automatic"));
    m_auto_apply_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_auto_apply_btn->SetToolTip(_L("Fill the rows with a freshly computed automatic match."));
    m_auto_apply_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_apply_automatic, this);
    bottom_sizer->Add(m_auto_apply_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

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

void FilamentMapDialog::rebuild_rows_panel(const std::vector<int> &proposal, bool auto_matched)
{
    // Clear(true) destroys the previous panel (and any scroll wrapper) along with its sizer
    // item; m_rows_panel itself is about to be reassigned below.
    m_rows_holder_sizer->Clear(true);

    m_rows_panel = new FilamentMapRowsPanel(this, m_filament_color, m_filament_type, m_filament_names,
                                            m_plate_filaments, proposal, m_inventory, build_slot_preset_names(), m_tool_count,
                                            auto_matched);
    // Orca: every rebuild starts disarmed -- on_apply_automatic re-arms it right after calling
    // this, so a rebuild for any other reason (ctor, Reset, an inventory edit) always lands
    // disarmed. See m_live_auto_pending's doc.
    m_live_auto_pending = false;
    m_rows_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        // A real tile pick (this panel's own construction above never fires this event) --
        // disarm live-auto: from here on OK saves a frozen map again, same as today.
        m_live_auto_pending = false;
        seed_rows_mode_status();
        event.Skip();
    });

    wxWindow *rows_widget = m_rows_panel;
    if (m_plate_filaments.size() > MAX_VISIBLE_MAP_ROWS) {
        // Orca: follows FilamentPickerDialog::CreateColorGrid's scrolled-window pattern -- cap
        // the visible height to MAX_VISIBLE_MAP_ROWS tile rows and let the rest scroll. per_row
        // is the fixed wrapped-tile-row height (see TILE_ROW_HEIGHT_DIP), not derived from the
        // panel's total best-size, which mixes in the footer/warning labels below the tile grid
        // and (pre-fix) assumed one row per filament.
        wxSize best    = m_rows_panel->GetBestSize();
        int    per_row = FromDIP(TILE_ROW_HEIGHT_DIP);

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

    update_mismatch_warning();
    update_preview();

    Layout();
}

std::map<int, std::string> FilamentMapDialog::build_slot_preset_names() const
{
    std::map<int, std::string> names;
    const PresetCollection &filament_presets = wxGetApp().preset_bundle->filaments;
    for (const auto &slots : m_inventory.tools)
        for (const PhysicalFilament &pf : slots) {
            if (pf.empty() || pf.id <= 0) continue;
            // Orca: shared slot_display_name (FilamentInventoryStore) -- the combo rows are
            // width-limited and the printer suffix is redundant inside a printer-scoped dialog.
            // The mismatch warning resolves names independently, so this map is display-only.
            names[pf.id] = slot_display_name(pf, filament_presets);
        }
    return names;
}

void FilamentMapDialog::update_mismatch_warning()
{
    if (!m_rows_panel) return;

    // Orca: non-blocking warning only (spec: mapping dialog shows slot profiles and warns on
    // merged-profile mismatch) -- a project filament's preset that differs from the physical
    // slot it's been merged onto is surfaced, never rejected.
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::unordered_map<int, int> counts;
    for (int id : physical_map)
        if (id > 0) ++counts[id];

    wxString warning;
    for (size_t i = 0; i < physical_map.size() && warning.empty(); ++i) {
        int id = physical_map[i];
        if (id <= 0 || counts[id] < 2) continue; // only filaments actually merged together matter here

        int f = (int) i + 1;
        std::string project_preset = ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string();
        if (project_preset.empty()) continue;

        const PhysicalFilament *pf = m_inventory.find(id);
        std::string slot_preset = pf ? resolve_slot_preset(*pf, wxGetApp().preset_bundle->filaments) : std::string();
        if (slot_preset.empty() || slot_preset == project_preset) continue;

        // Orca: no leading " · " -- this now renders on its own line (FilamentMapRowsPanel::
        // m_warning_label), not appended after the stats line.
        warning = wxString::Format(_L("Profile mismatch: %s ≠ %s"), from_u8(project_preset), from_u8(slot_preset));
    }

    m_rows_panel->SetFooterWarning(warning);
}

void FilamentMapDialog::update_preview()
{
    if (!m_preview_panel || !m_rows_panel)
        return;

    // Orca: a simplified take on SyncAmsInfoDialog's "after mapping" thumbnail --
    // recolors the CURRENT plate's cached render by this dialog's current row picks. Borrows the
    // core recolor formula from SyncAmsInfoDialog::change_default_normal (same no-light-alpha
    // filament-id decode, same lit-vs-flat brightness ratio to keep shading), but skips that
    // pipeline's edge-pixel smoothing pass (record_edge_pixels_data/final_deal_edge_pixels_data):
    // anti-aliased boundary pixels keep their un-recolored blend instead of being averaged from
    // neighbors -- a minor cosmetic difference, not visible without zooming in. Only the current
    // plate's "after" view is shown, no original/twin side, since this dialog only edits one plate.
    // Orca: the construction-time snapshot only (see the ctor) -- this method renders
    // nothing and touches no plate state, so it is safe to call from anywhere in the dialog's
    // life, including its own constructor and its modal loop. When no snapshot was available the
    // preview stays hidden; the dialog is fully usable without it.
    const ThumbnailData &lit      = m_preview_lit;
    const ThumbnailData &no_light = m_preview_no_light;
    if (!lit.is_valid() || !no_light.is_valid() || lit.width != no_light.width || lit.height != no_light.height) {
        m_preview_panel->Hide();
        Layout();
        return;
    }

    // 0-based project filament index -> physical filament id (0 = no pick, or bootstrap mode
    // where there's no physical color yet -- either way, that filament's pixels keep their
    // original lit color below).
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::vector<bool>     has_target(physical_map.size(), false);
    std::vector<wxColour> target_color(physical_map.size());
    for (size_t i = 0; i < physical_map.size(); ++i) {
        if (physical_map[i] <= 0) continue;
        const PhysicalFilament *pf = m_inventory.find(physical_map[i]);
        if (!pf || pf->color.empty()) continue;
        wxColour c(pf->color);
        if (!c.IsOk()) continue;
        // adjust_color_for_rendering matches the gamma handling the thumbnail's own GL render
        // already applied to every other volume (GLCanvas3D::render_thumbnail_*, and
        // SyncAmsInfoDialog::adjust_color_for_render for the same reason).
        Slic3r::ColorRGBA rgba{c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f, 1.0f};
        Slic3r::ColorRGBA adjusted = adjust_color_for_rendering(rgba);
        target_color[i] = wxColour((unsigned char) (adjusted[0] * 255.0f), (unsigned char) (adjusted[1] * 255.0f),
                                    (unsigned char) (adjusted[2] * 255.0f));
        has_target[i] = true;
    }

    wxImage image((int) lit.width, (int) lit.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < lit.height; ++r) {
        unsigned int rr = (lit.height - 1 - r) * lit.width;
        for (unsigned int c = 0; c < lit.width; ++c) {
            const unsigned char *origin_px   = lit.pixels.data() + 4 * (rr + c);
            const unsigned char *no_light_px = no_light.pixels.data() + 4 * (rr + c);
            unsigned char        out[3]      = {origin_px[0], origin_px[1], origin_px[2]};

            if (origin_px[3] > 0) {
                // Orca: GLCanvas3D's flat/no-light thumbnail pass encodes each pixel's source
                // filament (0-based) as 255 - alpha (see the ban_light branch setting
                // new_color[3] = (255 - (vol->extruder_id - 1)) / 255.0f) -- the same convention
                // SyncAmsInfoDialog::change_default_normal decodes.
                int filament_idx = 255 - no_light_px[3];
                if (filament_idx >= 0 && (size_t) filament_idx < has_target.size() && has_target[filament_idx]) {
                    const wxColour &target       = target_color[filament_idx];
                    int             origin_rgb   = origin_px[0] + origin_px[1] + origin_px[2];
                    int             no_light_rgb = no_light_px[0] + no_light_px[1] + no_light_px[2];
                    if (no_light_rgb <= 0) {
                        out[0] = target.Red(); out[1] = target.Green(); out[2] = target.Blue();
                    } else if (origin_rgb >= no_light_rgb) {
                        out[0] = (unsigned char) std::clamp(target.Red()   + (origin_px[0] - no_light_px[0]), 0, 255);
                        out[1] = (unsigned char) std::clamp(target.Green() + (origin_px[1] - no_light_px[1]), 0, 255);
                        out[2] = (unsigned char) std::clamp(target.Blue()  + (origin_px[2] - no_light_px[2]), 0, 255);
                    } else {
                        float ratio = origin_rgb / (float) no_light_rgb;
                        out[0] = (unsigned char) std::clamp((int) (target.Red()   * ratio), 0, 255);
                        out[1] = (unsigned char) std::clamp((int) (target.Green() * ratio), 0, 255);
                        out[2] = (unsigned char) std::clamp((int) (target.Blue()  * ratio), 0, 255);
                    }
                }
            }
            image.SetRGB((int) c, (int) r, out[0], out[1], out[2]);
            image.SetAlpha((int) c, (int) r, origin_px[3]);
        }
    }

    const wxSize target_size = m_preview_panel->GetMinSize();
    if (target_size.GetWidth() > 0 && target_size.GetHeight() > 0)
        image = image.Rescale(target_size.GetWidth(), target_size.GetHeight(), wxIMAGE_QUALITY_BOX_AVERAGE);
    m_preview_panel->set_thumbnail(image);
    m_preview_panel->Show();
    Layout();
}

void FilamentMapDialog::on_reset_to_initial(wxCommandEvent &)
{
    // Orca: reapply the construction-time proposal to the rows --
    // selections, tile wheels, auto-matched badges, and OK-gating -- without touching the plate
    // or closing. Reusing rebuild_rows_panel keeps this on the exact same path the ctor and
    // on_edit_inventory use to seed rows from a proposal, so all three stay in sync by
    // construction; m_initial_proposal_was_auto replays the same badge state the dialog opened
    // with (see its own doc), not just the same target picks.
    rebuild_rows_panel(m_initial_proposal, m_initial_proposal_was_auto);
    seed_rows_mode_status();
}

void FilamentMapDialog::on_apply_automatic(wxCommandEvent &)
{
    // Orca: the "automatic" escape hatch, distinct from Reset, which only reverts to the dialog's
    // initial state (see on_reset_to_initial). Recomputes a proposal the same way a plate's
    // first-ever (never confirmed) visit to this dialog would
    // (stored_map_confirmed=false ignores m_physical_filament_map/m_filament_map entirely, pure
    // auto-match), and shows it in the rows for review/adjustment.
    std::vector<int> auto_proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor, m_filament_preset, m_plate_filaments,
                                                                    m_physical_filament_map, m_filament_map, m_inventory,
                                                                    /*stored_map_confirmed=*/false);
    // auto_matched=true: this proposal is entirely fresh auto-matcher output, so every row it
    // assigns starts with its auto-matched badge shown.
    rebuild_rows_panel(auto_proposal, /*auto_matched=*/true);
    // Orca: arm live-auto -- rebuild_rows_panel above just disarmed it
    // (every rebuild starts disarmed), this button re-arms it right after. OK now, with no
    // manual tile pick in between, saves the plate in live mode instead of a frozen one; see
    // get_mode()/m_live_auto_pending's docs.
    m_live_auto_pending = true;
    seed_rows_mode_status();
}

FilamentInventory& FilamentMapDialog::device()
{
    return current_inventory_for_preset(active_printer_session().profile(), m_store, m_tool_count);
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
    bool edited = editor.ShowModal() == wxID_OK;

    // Orca: the device identity is this dialog's printer preset now, which the editor can no
    // longer switch out from under it (its device combo was dropped). Reload
    // the store/inventory and re-derive the record-offer gate (m_inventory_was_all_unset)
    // unconditionally anyway -- the editor's "Sync from printer" write-through
    // (FilamentInventoryEditor::do_sync_from_printer) can update this same device's inventory on
    // disk even when the modal result is Cancel, so a plain reload is still needed either way.
    m_store                    = load_filament_inventories();
    m_inventory                = device();
    m_inventory_was_all_unset = inventory_all_unset(m_inventory);

    if (edited) {
        // Treat the user's current picks as a confirmed stored map for re-proposing: a picked
        // row keeps its physical filament id (or, in bootstrap mode, its tool) when it still
        // resolves to something real after the edit; anything else (unassigned, or a slot the
        // edit just cleared/removed) falls back to auto-match, exactly like
        // compute_physical_map_proposal's stale-id case.
        std::vector<int> proposal = compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor, m_filament_preset, m_plate_filaments,
                                                                   current_physical, current_tool, m_inventory,
                                                                   /*stored_map_confirmed=*/true);
        // auto_matched=false: this reproposes the user's own current picks (stored_map_confirmed
        // above), not a fresh auto-matcher pass, so no row here should start badged.
        rebuild_rows_panel(proposal, /*auto_matched=*/false);
    }

    seed_rows_mode_status();
}

FilamentMapMode FilamentMapDialog::get_mode()
{
    // While live-auto is armed (Automatic pressed, no manual tile pick
    // since -- see m_live_auto_pending's doc), report fmmDefault instead of the usual frozen
    // fmmManual, so the caller saves this plate to be re-mapped fresh on every slice.
    if (m_rows_panel) return m_live_auto_pending ? fmmDefault : fmmManual;
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
            // Load-modify-save: start from what's on disk so this write can never discard
            // swappable rows recorded via the Physical Filaments editor (this offer only shows
            // for an all-unset inventory, but the disk may have moved since the dialog was
            // built), and keep any id a loaded slot already owns so existing physical-map
            // references stay valid. ensure_ids mints ids for the slots this write fills.
            m_store = load_filament_inventories();
            FilamentInventory &new_inv = device();
            for (int f : m_plate_filaments) {
                if (f < 1 || f > (int) m_filament_map.size()) continue;
                int tool = m_filament_map[f - 1];
                if (tool < 1 || tool > (int) m_tool_count) continue;
                LoadedFilament &slot = new_inv.tools[tool - 1][0];
                // Orca: shared with FilamentInventoryEditor::slot_from_row -- preset is now
                // set here too (from m_filament_preset), so a slot recorded through this bootstrap
                // offer resolves a preset/vendor/mismatch-warning identically to one recorded
                // through the editor, instead of rendering as a bare type forever.
                slot = build_physical_filament(
                    (f - 1 < (int) m_filament_color.size()) ? m_filament_color[f - 1] : std::string(),
                    (f - 1 < (int) m_filament_type.size()) ? m_filament_type[f - 1] : std::string(),
                    ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string(),
                    slot.id, slot.kind);
            }
            new_inv.ensure_ids();
            save_filament_inventories(m_store);

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
