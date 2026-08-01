#include "FilamentInventoryEditor.hpp"

#include <wx/clrpicker.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "FilamentInventoryStore.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

// Orca: neutral placeholder for a slot with no color recorded yet. Distinct from the "unset"
// signal itself -- that's carried by Row::color_touched/type_touched and the Clear/Remove
// button's enabled state -- this is just what the picker shows so an empty slot doesn't look
// like a real black/white pick.
static const wxColour UNSET_COLOR(0xD9, 0xD9, 0xD9);

FilamentInventoryEditor::FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count)
    : wxDialog(parent, wxID_ANY, _L("Physical Filaments"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_printer_preset_name(printer_preset_name)
{
    SetBackgroundColour(*wxWHITE);

    m_type_values = print_config_def.get("filament_type")->enum_values;

    // load_filament_inventory already sizes inv.tools to tool_count with slot 0 present per tool
    // (FilamentInventory::deserialize's invariant). This is the load-modify-save working model:
    // every row's id/kind is copied from it verbatim and never touched again unless the row is
    // brand new (added via "Add filament" in this session, see Row::is_new).
    FilamentInventory inv = load_filament_inventory(printer_preset_name, tool_count);
    m_next_id = inv.next_id;

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(15));

    m_tools.resize(tool_count);
    for (size_t i = 0; i < tool_count; ++i) {
        ToolGroup& group = m_tools[i];

        // Seed the row data (not widgets yet -- rebuild_tool_rows below creates those) from every
        // slot of this tool, loaded slot first (slot 0), matching the store's invariant.
        group.rows.resize(inv.tools[i].size());
        for (size_t s = 0; s < inv.tools[i].size(); ++s) {
            const PhysicalFilament& slot = inv.tools[i][s];
            Row& row = group.rows[s];
            row.id            = slot.id;
            row.is_new        = false;
            row.kind          = slot.kind;
            row.color_touched = !slot.color.empty();
            row.type_touched  = !slot.type.empty();
            if (row.color_touched) {
                wxColour c(slot.color);
                if (c.IsOk())
                    row.last_color = c;
            }
            if (row.type_touched) {
                for (size_t t = 0; t < m_type_values.size(); ++t) {
                    if (m_type_values[t] == slot.type) { row.last_type_sel = (int) t + 1; break; }
                }
            }
        }

        auto* label = new Label(this, wxString::Format(_L("Tool %d"), (int) i + 1));
        main_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

        group.rows_sizer = new wxBoxSizer(wxVERTICAL);
        main_sizer->Add(group.rows_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        auto* add_btn = new Button(this, _L("Add filament"));
        add_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        add_btn->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { add_filament(i); });
        main_sizer->Add(add_btn, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        main_sizer->AddSpacer(FromDIP(15));

        // Reconstruct row widgets straight from the seeded data above; this is the same path
        // add_filament()/remove_filament() use, so initial layout and post-edit layout share one
        // code path.
        rebuild_tool_rows(i);
    }

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->AddStretchSpacer();

    // Orca: "Sync from printer" is the leftmost entry; OK/Cancel stay last. left_aligned_buttons_count
    // = 1 keeps it pinned left of the stretch spacer while OK/Cancel are pushed right as usual.
    auto* dlg_btns = new DialogButtons(bottom_panel, {"Sync from printer", "OK", "Cancel"}, "", 1);
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    // Orca: only offer sync when a connected machine actually supports it (R2.1 "Sync from
    // printer" -- best-effort, offline editing stays the baseline). Recomputed once here; if the
    // machine connects/disconnects while the dialog is open the button state won't follow it.
    Button* sync_btn = dlg_btns->GetButtonFromIndex(0);
    NetworkAgent*  net_agent = wxGetApp().getAgent();
    DeviceManager* dev_mgr   = wxGetApp().getDeviceManager();
    MachineObject* machine   = dev_mgr ? dev_mgr->get_selected_machine() : nullptr;
    bool sync_available = net_agent && machine && machine->is_connected() &&
                          net_agent->get_filament_sync_mode() != FilamentSyncMode::none;
    sync_btn->Enable(sync_available);
    if (!sync_available)
        sync_btn->SetToolTip(_L("Connect to a printer that supports reading loaded filaments to use this."));
    sync_btn->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_sync_from_printer, this);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &FilamentInventoryEditor::on_ok, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    SetEscapeId(wxID_CANCEL);

    SetSizer(main_sizer);
    Layout();
    Fit();

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentInventoryEditor::update_clear_enabled(Row& row)
{
    row.action_btn->Enable(row.color_touched || row.type_touched);
}

void FilamentInventoryEditor::rebuild_tool_rows(size_t tool_idx)
{
    ToolGroup& group = m_tools[tool_idx];

    // Capture each row's current widget state into plain data *before* destroying the widgets
    // below -- add_row_widgets() reads these to seed the recreated widgets, since reading from an
    // already-destroyed wxColourPickerCtrl/ComboBox would be use-after-free.
    for (Row& row : group.rows) {
        if (row.color_picker)
            row.last_color = row.color_picker->GetColour();
        if (row.type_choice)
            row.last_type_sel = row.type_choice->GetSelection();
        row.color_picker = nullptr;
        row.type_choice  = nullptr;
        row.action_btn   = nullptr;
    }

    // Destroys every row widget currently in this tool's rows_sizer; Row::id/is_new/kind/
    // color_touched/type_touched/last_color/last_type_sel live in group.rows and are untouched by
    // this.
    group.rows_sizer->Clear(true);

    for (size_t r = 0; r < group.rows.size(); ++r)
        add_row_widgets(tool_idx, r);

    Layout();
    Fit();
}

void FilamentInventoryEditor::add_row_widgets(size_t tool_idx, size_t row_idx)
{
    ToolGroup& group = m_tools[tool_idx];
    Row& row = group.rows[row_idx];
    const bool is_loaded = (row_idx == 0); // slot 0 = loaded, always present, Clear not Remove

    wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

    row.color_picker = new wxColourPickerCtrl(this, wxID_ANY, row.last_color);

    row.type_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, nullptr, wxCB_READONLY);
    row.type_choice->Append(""); // blank = unknown/unset
    for (const std::string& t : m_type_values)
        row.type_choice->Append(t);
    row.type_choice->SetSelection(row.type_touched ? row.last_type_sel : 0);

    if (is_loaded) {
        auto* clear_btn = new Button(this, _L("Clear"));
        clear_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        row.action_btn = clear_btn;
    } else {
        auto* remove_btn = new ScalableButton(this, wxID_ANY, "cross", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true, 16);
        remove_btn->SetToolTip(_L("Remove filament"));
        row.action_btn = remove_btn;
    }

    row.color_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, tool_idx, row_idx](wxColourPickerEvent&) {
        Row& r = m_tools[tool_idx].rows[row_idx];
        r.color_touched = true;
        if (row_idx == 0)
            update_clear_enabled(r);
    });
    row.type_choice->Bind(wxEVT_COMBOBOX, [this, tool_idx, row_idx](wxCommandEvent&) {
        Row& r = m_tools[tool_idx].rows[row_idx];
        r.type_touched = r.type_choice->GetSelection() > 0;
        if (row_idx == 0)
            update_clear_enabled(r);
    });
    if (is_loaded) {
        row.action_btn->Bind(wxEVT_BUTTON, [this, tool_idx](wxCommandEvent&) {
            Row& r = m_tools[tool_idx].rows[0];
            r.color_picker->SetColour(UNSET_COLOR);
            r.type_choice->SetSelection(0);
            r.color_touched = false;
            r.type_touched  = false;
            update_clear_enabled(r);
        });
        update_clear_enabled(row);
    } else {
        row.action_btn->Bind(wxEVT_BUTTON, [this, tool_idx, row_idx](wxCommandEvent&) { remove_filament(tool_idx, row_idx); });
    }

    row_sizer->Add(row.color_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(row.type_choice, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row_sizer->Add(row.action_btn, 0, wxALIGN_CENTER_VERTICAL);

    group.rows_sizer->Add(row_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
}

void FilamentInventoryEditor::add_filament(size_t tool_idx)
{
    ToolGroup& group = m_tools[tool_idx];
    Row row;
    row.id     = 0; // minted from the next_id allocator at OK/save time, see Row::is_new
    row.is_new = true;
    row.kind   = PhysicalFilament::Kind::Manual;
    group.rows.push_back(row);
    rebuild_tool_rows(tool_idx);
}

void FilamentInventoryEditor::remove_filament(size_t tool_idx, size_t row_idx)
{
    ToolGroup& group = m_tools[tool_idx];
    if (row_idx == 0 || row_idx >= group.rows.size())
        return; // slot 0 (loaded) can never be removed, only cleared
    group.rows.erase(group.rows.begin() + row_idx);
    rebuild_tool_rows(tool_idx);
}

void FilamentInventoryEditor::on_sync_from_printer(wxCommandEvent&)
{
    // Orca: best-effort sync (R2.1). Any failure is non-blocking and leaves the editor
    // untouched -- offline manual editing stays the baseline workflow. Sync only ever targets
    // each tool's slot-0 (loaded) row; swappable rows and their ids are left exactly as they
    // were, since the printer only ever reports what's currently loaded.
    NetworkAgent* net_agent = wxGetApp().getAgent();
    DeviceManager* dev_mgr  = wxGetApp().getDeviceManager();
    MachineObject* machine  = dev_mgr ? dev_mgr->get_selected_machine() : nullptr;
    if (!net_agent || !machine) {
        MessageDialog(this, _L("No connected printer is available to sync from."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    // fetch_filament_info() is a synchronous/blocking REST call (see IPrinterAgent.hpp); pull-mode
    // agents populate the machine's DevFilaSystem before returning. Subscription-mode agents keep
    // DevFilaSystem current via MQTT pushes already, so there's nothing to fetch here. Mirrors the
    // existing pattern in Sidebar::build_filament_ams_list/load_ams_list (Plater.cpp).
    if (net_agent->get_filament_sync_mode() == FilamentSyncMode::pull) {
        if (!net_agent->fetch_filament_info(machine->get_dev_id())) {
            MessageDialog(this, _L("Failed to read filament info from the printer."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
            return;
        }
    }

    std::shared_ptr<DevFilaSystem> fila_system = machine->GetFilaSystem();
    std::map<int, DevAmsSlotId> tray_map = fila_system ? fila_system->GetTrayIndexMap() : std::map<int, DevAmsSlotId>();

    // Orca: tray -> physical tool correspondence is ambiguous for toolchangers (DevFilaSystem's
    // trays are AMS-shaped, not tool-shaped). Best-effort: real AMS trays, sorted by tray index,
    // map 1:1 onto tool rows up to tool_count. Rows beyond the reported tray count, and trays
    // beyond the row count, are left alone.
    //
    // GetTrayIndexMap() unconditionally seeds two virtual/external-spool pseudo-tray entries
    // (VIRTUAL_TRAY_MAIN_ID/DEPUTY_ID) alongside any real AMS trays, so the map is never actually
    // empty -- it can't be used as an "any data?" signal. Those pseudo-trays live in
    // MachineObject::vt_slot, not in DevFilaSystem's amsList, so GetAmsTray() below can never
    // resolve them anyway; skip them explicitly (rather than relying on that null result) so they
    // don't silently consume a tool row on AMS-less toolchangers, and count rows actually applied
    // to know whether the sync produced anything.
    size_t tool_idx     = 0;
    size_t rows_applied = 0;
    for (const auto& [tray_index, slot_id] : tray_map) {
        if (devPrinterUtil::IsVirtualSlot(slot_id.first))
            continue;
        if (tool_idx >= m_tools.size())
            break;
        Row& row = m_tools[tool_idx].rows[0]; // slot 0 = loaded; sync never touches swappable rows
        ++tool_idx;

        DevAmsTray* tray = fila_system->GetAmsTray(std::to_string(slot_id.first), std::to_string(slot_id.second));
        if (!tray || !tray->is_exists)
            continue;

        bool applied = false;
        if (!tray->color.empty()) {
            row.color_picker->SetColour(tray->get_color());
            row.color_touched = true;
            applied = true;
        }
        const std::string type = tray->get_filament_type();
        if (!type.empty()) {
            for (size_t t = 0; t < m_type_values.size(); ++t) {
                if (m_type_values[t] == type) {
                    row.type_choice->SetSelection((int) t + 1);
                    row.type_touched = true;
                    applied = true;
                    break;
                }
            }
        }
        if (applied) {
            update_clear_enabled(row);
            ++rows_applied;
        }
    }

    if (rows_applied == 0)
        MessageDialog(this, _L("The printer did not report any filament information."), _L("Sync from printer"), wxOK | wxICON_INFORMATION).ShowModal();
}

void FilamentInventoryEditor::on_ok(wxCommandEvent&)
{
    // Load-modify-save: every row's id is either the one it already had (rows loaded from disk,
    // including untouched ones -- never reassigned here) or, for a brand-new row added this
    // session (Row::is_new), minted now from the next_id allocator. This is the only place ids
    // are minted; a cancelled dialog never advances the on-disk allocator.
    FilamentInventory inv;
    inv.tools.resize(m_tools.size());
    for (size_t i = 0; i < m_tools.size(); ++i) {
        const ToolGroup& group = m_tools[i];
        inv.tools[i].resize(group.rows.size());
        for (size_t r = 0; r < group.rows.size(); ++r) {
            const Row& row = group.rows[r];
            PhysicalFilament& slot = inv.tools[i][r];
            slot.kind = row.kind;
            if (row.color_touched)
                slot.color = row.color_picker->GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
            if (row.type_touched)
                slot.type = row.type_choice->GetString((unsigned) row.type_choice->GetSelection()).ToStdString();
            slot.id = row.is_new ? m_next_id++ : row.id;
        }
    }
    inv.next_id = m_next_id;
    save_filament_inventory(m_printer_preset_name, inv);
    EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI
