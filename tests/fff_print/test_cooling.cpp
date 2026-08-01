#include <catch2/catch_all.hpp>

#include "test_helpers.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// The fan is held off for the first close_fan_the_first_x_layers layers, so an explicit
// fan-off command is emitted.
TEST_CASE("Fan is held off for the initial layers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "cooling",                      true },
        { "close_fan_the_first_x_layers", 5 },
    });
    CHECK(gcode.find("M106 S0") != std::string::npos);
}

// The cooling pass resolves and strips its internal speed placeholders; none leak into
// the final G-code.
TEST_CASE("Cooling consumes its internal speed markers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, { { "layer_height", 0.2 } });
    CHECK(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
}

// Regression: CoolingBuffer parsed a mapped printer's T command (a physical tool number) as if it
// were already a filament id, both when bucketing moves into PerExtruderAdjustments
// (parse_layer_gcode) and when picking which filament's fan settings apply
// (apply_layer_cooldown's EXTRUDER_CONFIG lookups, keyed by m_current_extruder). On a printer
// with fewer filaments than tools, a physical tool number for a used tool can exceed the
// filament count, so CoolingBuffer flagged it as an out-of-range/invalid toolchange and silently
// skipped applying that filament's per-material cooling settings - two filaments always ended up
// sharing whichever filament's config happened to occupy that (wrong) slot, or the toolchange was
// ignored outright.
//
// filament 1 -> physical tool 3, filament 2 -> physical tool 4 (both indices exceed the 2-filament
// count). initial_layer_fan_speed forces an exact, layer-time-independent fan percentage on layer
// 0, so the M106 value is a direct, deterministic readout of which filament's config applied.
TEST_CASE("Mapped printer's per-filament fan settings survive physical tool numbers", "[Cooling][MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(2, {
            { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
            { "single_extruder_multi_material",  "0" },
            { "enable_filament_mapping",         "1" },
            { "filament_map_mode",               "Manual" },
            { "filament_map",                    "3,4" },   // filament 1 -> tool 3, filament 2 -> tool 4
            { "wall_filament",                   "1" },
            { "sparse_infill_filament",           2 },
            { "solid_infill_filament",             2 },
            { "enable_prime_tower",               "0" },
            { "close_fan_the_first_x_layers",    "0,0" },
            { "initial_layer_fan_speed",         "20,80" },  // filament 1: 20%, filament 2: 80%
        }));
    // 255.5 * speed / 100, truncated: 20% -> 51, 80% -> 204.
    CHECK(gcode.find("M106 S51")  != std::string::npos);   // filament 1's setting, on physical tool 3 (T2)
    CHECK(gcode.find("M106 S204") != std::string::npos);   // filament 2's setting, on physical tool 4 (T3)
}
