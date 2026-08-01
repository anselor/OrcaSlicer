#ifndef slic3r_GUI_FilamentInventoryEditor_hpp_
#define slic3r_GUI_FilamentInventoryEditor_hpp_

#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/dialog.h>

#include "libslic3r/FilamentInventory.hpp"

class wxColourPickerCtrl;
class wxBoxSizer;
class ComboBox;
class Button;

namespace Slic3r { namespace GUI {

// "Physical Filaments" editor: per tool, the currently loaded physical filament (slot 0) plus
// zero or more swappable ones (Slic3r::FilamentInventory, libslic3r/FilamentInventory.hpp), so
// the mapping dialog (FilamentMapDialog) can target a specific physical filament -- not just a
// tool -- when auto-suggesting or letting the user pick a plate-filament assignment.
// Persistence goes through GUI::load_filament_inventory / save_filament_inventory
// (FilamentInventoryStore.hpp) and follows load-modify-save: the inventory loaded from disk is
// kept as the working model for the whole session, edited in place, and written back unchanged
// except for the edits the user actually made -- ids of rows the user didn't touch never change,
// since plate mappings reference physical filaments by id across sessions.
class FilamentInventoryEditor : public wxDialog
{
public:
    FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count);

private:
    // One physical filament row: the tool's loaded slot (slot 0, always present, has a Clear
    // button) or a swappable slot (has a Remove button instead). color_touched/type_touched are
    // tracked independently -- picking only a type must not also persist the untouched picker's
    // neutral placeholder color as if it were a real color pick, and vice versa.
    //
    // is_new is true only for rows created via "Add filament" in this session; such a row's `id`
    // stays 0 until OK, when it is minted from the inventory's next_id allocator. Every row
    // loaded from disk has is_new == false and keeps whatever `id` it already had (including 0
    // for a never-set loaded slot) for the rest of the session, regardless of edits -- ids are
    // never reassigned to a pre-existing row.
    struct Row
    {
        int                   id{0};
        bool                  is_new{false};
        Slic3r::PhysicalFilament::Kind kind{Slic3r::PhysicalFilament::Kind::Manual};
        bool                  color_touched{false};
        bool                  type_touched{false};
        // Data source of truth for the widgets, captured just before a rebuild destroys them
        // (rebuild_tool_rows) and used to seed the freshly recreated ones (add_row_widgets) --
        // reading a destroyed widget's old value would be use-after-free.
        wxColour              last_color{0xD9, 0xD9, 0xD9};
        int                   last_type_sel{0};
        wxColourPickerCtrl*   color_picker{nullptr};
        ComboBox*             type_choice{nullptr};
        wxWindow*             action_btn{nullptr}; // Clear (slot 0) or Remove (swappable)
    };

    // A tool's rows (rows[0] = loaded, rows[1..] = swappable) plus the sizer that gets rebuilt
    // (widgets destroyed and recreated) whenever a row is added or removed. Row::id/is_new/
    // kind/color_touched/type_touched are plain data and survive a rebuild; the widget pointers
    // are recreated by it, so any lambda capturing a row index must look the row back up by
    // index rather than caching a Row* across a rebuild.
    struct ToolGroup
    {
        std::vector<Row> rows;
        wxBoxSizer*       rows_sizer{nullptr}; // vertical, one horizontal row-sizer per entry
    };

    void on_ok(wxCommandEvent& event);
    void on_sync_from_printer(wxCommandEvent& event);
    void update_clear_enabled(Row& row);
    void rebuild_tool_rows(size_t tool_idx);
    void add_row_widgets(size_t tool_idx, size_t row_idx);
    void add_filament(size_t tool_idx);
    void remove_filament(size_t tool_idx, size_t row_idx);

    std::string               m_printer_preset_name;
    std::vector<ToolGroup>    m_tools;
    std::vector<std::string>  m_type_values; // filament_type enum values, in ComboBox order
    int                       m_next_id{1};   // FilamentInventory::next_id at load; advances only at save
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryEditor_hpp_
