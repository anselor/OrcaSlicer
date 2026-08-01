#include "FilamentInventoryStore.hpp"

#include "GUI_App.hpp"

namespace Slic3r { namespace GUI {

static const char *LOADED_FILAMENTS_KEY = "loaded_filaments";

FilamentInventory load_filament_inventory(const std::string &printer_preset_name, size_t tool_count)
{
    std::string serialized = wxGetApp().app_config->get_printer_setting(printer_preset_name, LOADED_FILAMENTS_KEY);
    return FilamentInventory::deserialize(serialized, tool_count);
}

void save_filament_inventory(const std::string &printer_preset_name, const FilamentInventory &inv)
{
    wxGetApp().app_config->set_printer_setting(printer_preset_name, LOADED_FILAMENTS_KEY, inv.serialize());
}

}} // namespace Slic3r::GUI
