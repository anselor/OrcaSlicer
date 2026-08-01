#ifndef slic3r_GUI_FilamentInventoryStore_hpp_
#define slic3r_GUI_FilamentInventoryStore_hpp_

#include <string>

#include "libslic3r/FilamentInventory.hpp"

namespace Slic3r { namespace GUI {

// Thin persistence layer for the per-machine "what's currently loaded in each physical tool"
// inventory (Slic3r::FilamentInventory, src/libslic3r/FilamentInventory.hpp). Stored via
// AppConfig's per-printer settings (wxGetApp().app_config->get/set_printer_setting), keyed by
// printer preset name, under the "loaded_filaments" key. The value surviving
// PresetBundle::export_selections()'s clear+rewrite of those settings is handled separately in
// PresetBundle.cpp, on the raw string, since PresetBundle is libslic3r and must not include this
// GUI header.
FilamentInventory load_filament_inventory(const std::string &printer_preset_name, size_t tool_count);
void              save_filament_inventory(const std::string &printer_preset_name, const FilamentInventory &inv);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryStore_hpp_
