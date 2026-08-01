#include "FilamentMapRowsPanel.hpp"
#include "FilamentMapPanel.hpp" // wxEVT_INVALID_MANUAL_MAP
#include "DragDropPanel.hpp"    // Hex2Color
#include "GUI_App.hpp"
#include "wxExtensions.hpp"

#include <wx/statbmp.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r { namespace GUI {

static const wxColour TextNormalGreyColor = wxColour("#6B6B6B");
static const wxColour TextErrorColor      = wxColour("#E14747");

FilamentMapRowsPanel::FilamentMapRowsPanel(wxWindow                        *parent,
                                           const std::vector<std::string> &filament_colors,
                                           const std::vector<std::string> &filament_types,
                                           const std::vector<std::string> &filament_names,
                                           const std::vector<int>          &plate_filaments,
                                           const std::vector<int>          &proposal,
                                           const FilamentInventory          &inventory,
                                           size_t                            tool_count)
    : wxPanel(parent)
    , m_filament_count(filament_colors.size())
    , m_tool_count(tool_count)
{
    SetName(wxT("FilamentMapRowsPanel"));
    SetBackgroundColour(*wxWHITE);
    m_base_map.assign(m_filament_count, 1);
    m_base_physical_map.assign(m_filament_count, 0);

    BuildTargetOptions(inventory);

    const int swatch_size = FromDIP(24);
    auto      top_sizer   = new wxBoxSizer(wxVERTICAL);

    m_rows.reserve(plate_filaments.size());
    for (size_t row_i = 0; row_i < plate_filaments.size(); ++row_i) {
        int f = plate_filaments[row_i];
        if (f < 1 || f > (int) filament_colors.size()) continue;

        auto row_sizer = new wxBoxSizer(wxHORIZONTAL);

        wxBitmap *swatch = get_extruder_color_icon(filament_colors[f - 1], std::to_string(f), swatch_size, swatch_size);
        auto      swatch_bmp = new wxStaticBitmap(this, wxID_ANY, swatch ? *swatch : wxNullBitmap);
        row_sizer->Add(swatch_bmp, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->AddSpacer(FromDIP(8));

        auto type_label = new Label(this, Label::Body_13, from_u8(filament_types[f - 1]));
        row_sizer->Add(type_label, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->AddSpacer(FromDIP(8));

        auto name_label = new Label(this, Label::Body_13, from_u8(filament_names.size() > (size_t) (f - 1) ? filament_names[f - 1] : ""));
        name_label->SetForegroundColour(TextNormalGreyColor);
        name_label->SetMinSize(wxSize(FromDIP(160), -1));
        row_sizer->Add(name_label, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->AddStretchSpacer(1);

        auto tool_choice = new wxBitmapComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(220), -1), 0, nullptr, wxCB_READONLY);
        BuildToolChoices(tool_choice);
        int prop = (row_i < proposal.size()) ? proposal[row_i] : 0;
        int sel  = wxNOT_FOUND;
        if (m_bootstrap_mode) {
            // Orca: compute_physical_map_proposal sentinel-encodes a bootstrap tool pick as
            // -(1-based tool); decode back to the matching placeholder option's 0-based tool.
            if (prop < 0) {
                int tool = -prop - 1;
                for (size_t oi = 0; oi < m_target_options.size(); ++oi)
                    if (m_target_options[oi].tool == tool) { sel = (int) oi; break; }
            }
        } else if (prop > 0) {
            for (size_t oi = 0; oi < m_target_options.size(); ++oi)
                if (m_target_options[oi].id == prop) { sel = (int) oi; break; }
        }
        tool_choice->SetSelection(sel);
        tool_choice->Bind(wxEVT_COMBOBOX, &FilamentMapRowsPanel::OnRowChanged, this);
        row_sizer->Add(tool_choice, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->AddSpacer(FromDIP(8));

        auto warning = new Label(this, Label::Body_13, _L("pick a tool"));
        warning->SetForegroundColour(TextErrorColor);
        row_sizer->Add(warning, 0, wxALIGN_CENTER_VERTICAL);

        top_sizer->Add(row_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));

        m_rows.push_back({f, tool_choice, warning});
    }

    top_sizer->AddSpacer(FromDIP(10));
    m_footer = new Label(this, Label::Body_13);
    m_footer->SetForegroundColour(TextNormalGreyColor);
    top_sizer->Add(m_footer, 0, wxALIGN_LEFT);

    SetSizer(top_sizer);
    Layout();
    Fit();

    // Orca: seed the warning flags and footer text, but do NOT fire wxEVT_INVALID_MANUAL_MAP
    // here -- the owning dialog queries AllRowsAssigned() itself right after construction to
    // set the initial OK button state (see FilamentMapPanel.hpp's event contract).
    UpdateFooter();

    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapRowsPanel::BuildTargetOptions(const FilamentInventory &inventory)
{
    m_target_options.clear();

    // Orca: shares its "is there anything recorded" definition with FilamentMapDialog's record-
    // checkbox gating and libslic3r's compute_physical_map_proposal (same canonical
    // Slic3r::inventory_all_unset) so all three agree on when a plate is in bootstrap mode --
    // see that function's doc comment for why a loaded-only check would disagree here.
    m_bootstrap_mode = inventory_all_unset(inventory);

    if (m_bootstrap_mode) {
        // Orca: nothing physically recorded anywhere yet (a fresh machine profile) -- fall back
        // to picking a bare tool, same as the pre-physical-targets dialog, so the "Remember
        // these as the loaded filaments" offer on OK still has something to bootstrap from.
        for (size_t t = 0; t < m_tool_count; ++t) {
            TargetOption opt;
            opt.id    = 0;
            opt.tool  = (int) t;
            opt.label = wxString::Format(_L("Tool %d"), (int) t + 1);
            m_target_options.push_back(std::move(opt));
        }
        return;
    }

    const int icon_size = FromDIP(16);
    for (size_t t = 0; t < m_tool_count && t < inventory.tools.size(); ++t) {
        const auto &slots = inventory.tools[t];
        for (size_t si = 0; si < slots.size(); ++si) {
            const PhysicalFilament &pf = slots[si];
            // pf.id <= 0 here (non-empty but no valid id) shouldn't happen through any normal
            // save path (FilamentInventoryEditor::on_ok and the v1/v2 migrations always mint an
            // id for a non-empty slot) -- only reachable via a hand-edited/corrupted app config.
            // m_bootstrap_mode is already false (inventory_all_unset only looks at content), so
            // such a slot is silently omitted rather than falling back to bootstrap; that's the
            // safer of two bad options for corrupt data (no silent "record over it" offer either).
            if (pf.empty() || pf.id <= 0) continue; // nothing physically recorded there

            TargetOption opt;
            opt.id   = pf.id;
            opt.tool = (int) t;

            wxString label = wxString::Format(_L("Tool %d"), (int) t + 1);
            if (!pf.type.empty())
                label += wxString::Format(" – %s", from_u8(pf.type));
            label += (si == 0) ? _L(" (loaded)") : _L(" (swap)");
            opt.label = label;

            if (!pf.color.empty()) {
                wxBitmap *icon = get_extruder_color_icon(pf.color, "", icon_size, icon_size);
                if (icon) opt.bitmap = *icon;
            }

            m_target_options.push_back(std::move(opt));
        }
    }
}

void FilamentMapRowsPanel::BuildToolChoices(wxBitmapComboBox *choice) const
{
    for (const TargetOption &opt : m_target_options)
        choice->Append(opt.label, opt.bitmap);
}

bool FilamentMapRowsPanel::ResolveRow(const Row &row, int &tool, int &id) const
{
    int sel = row.tool_choice->GetSelection();
    if (sel == wxNOT_FOUND || sel < 0 || (size_t) sel >= m_target_options.size())
        return false;
    tool = m_target_options[sel].tool;
    id   = m_target_options[sel].id;
    return true;
}

void FilamentMapRowsPanel::OnRowChanged(wxCommandEvent &event)
{
    UpdateFooter();

    wxCommandEvent evt(wxEVT_INVALID_MANUAL_MAP);
    evt.SetInt(AllRowsAssigned() ? 1 : 0);
    ProcessEvent(evt);

    event.Skip();
}

void FilamentMapRowsPanel::ComputeStats(int &idle, int &swaps, int &merged) const
{
    std::vector<int>                    assigned_counts(m_tool_count, 0);
    std::vector<std::unordered_set<int>> tool_ids(m_tool_count);
    std::unordered_map<int, int>        id_counts;

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        ++assigned_counts[tool];
        if (m_bootstrap_mode) continue; // no physical identity to merge on yet
        tool_ids[tool].insert(id);
        ++id_counts[id];
    }

    idle  = 0;
    swaps = 0;
    for (size_t t = 0; t < m_tool_count; ++t) {
        if (assigned_counts[t] == 0) { ++idle; continue; }
        int distinct_targets = m_bootstrap_mode ? assigned_counts[t] : (int) tool_ids[t].size();
        swaps += std::max(0, distinct_targets - 1);
    }

    merged = 0;
    if (!m_bootstrap_mode)
        for (const auto &kv : id_counts)
            merged += std::max(0, kv.second - 1);
}

void FilamentMapRowsPanel::UpdateFooter()
{
    for (const Row &row : m_rows) {
        int tool, id;
        row.warning->Show(!ResolveRow(row, tool, id));
    }

    int idle, swaps, merged;
    ComputeStats(idle, swaps, merged);

    wxString text = wxString::Format(_L("%d tools idle · %d swaps on this plate"), idle, swaps);
    if (merged > 0)
        text += wxString::Format(_L(" · %d merged"), merged);
    m_footer->SetLabel(text);

    Layout();
    Fit();
    if (GetParent()) {
        GetParent()->Layout();
        GetParent()->Fit();
    }
}

void FilamentMapRowsPanel::SetBaseMap(const std::vector<int> &full_map) { m_base_map = full_map; }
void FilamentMapRowsPanel::SetBasePhysicalMap(const std::vector<int> &full_map) { m_base_physical_map = full_map; }

std::vector<int> FilamentMapRowsPanel::GetFilamentMaps() const
{
    std::vector<int> result(m_filament_count, 1);
    for (size_t i = 0; i < result.size() && i < m_base_map.size(); ++i)
        result[i] = m_base_map[i];

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = tool + 1;
    }
    return result;
}

std::vector<int> FilamentMapRowsPanel::GetPhysicalMaps() const
{
    std::vector<int> result(m_filament_count, 0);
    for (size_t i = 0; i < result.size() && i < m_base_physical_map.size(); ++i)
        result[i] = m_base_physical_map[i];

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        // A resolved normal-mode row always has id > 0 (a real physical filament); id <= 0 here
        // means this is a bootstrap row (no physical filament exists yet for this tool pick). Write
        // 0 rather than `continue` so a re-picked row clears any stale id left over from a previous
        // (e.g. merged) assignment -- otherwise the plate's old physical id for this slot survives
        // a bootstrap re-mapping that was meant to replace it.
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = id > 0 ? id : 0;
    }
    return result;
}

bool FilamentMapRowsPanel::AllRowsAssigned() const
{
    return std::all_of(m_rows.begin(), m_rows.end(), [this](const Row &row) {
        int tool, id;
        return ResolveRow(row, tool, id);
    });
}

int FilamentMapRowsPanel::SwapCount() const
{
    int idle, swaps, merged;
    ComputeStats(idle, swaps, merged);
    return swaps;
}

}} // namespace Slic3r::GUI
