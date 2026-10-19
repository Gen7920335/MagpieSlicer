#pragma once

#include <libslic3r/ModHost/ModSet.hpp>

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// The Mods page of a settings tab: one row per parameter the loaded mods store in this tab's
// preset (process, printer or filament). Edits go into the preset's mod value key
// (mod_settings, printer_mod_settings or filament_mod_settings) as JSON, so the preset is
// dirtied and saved like any other setting.
class ModParamsPanel : public wxPanel
{
public:
    // `scope` is "process", "printer" or "filament".
    ModParamsPanel(wxWindow *parent, std::shared_ptr<const ModHost::ModSet> mods, const std::string &scope);

    // True when the loaded mods have a parameter of `scope`, so the tab needs a Mods page.
    static bool has_params(const ModHost::ModSet *mods, const std::string &scope);
    // The config key that stores the values of `scope`.
    static const char *config_key(const std::string &scope);

    // Shows the values stored in `config` (defaults where none is stored).
    void load_from_config(const DynamicPrintConfig &config);
    // Called with the new JSON text after the user changes a value.
    std::function<void(const std::string &json_text)> on_change;
    // Shows and hides rows by their "visible" conditions. A condition may read a parameter another
    // tab stores, so the tabs call this on each other after a change.
    void update_visibility();

private:
    struct Row
    {
        std::string            key; // "<mod id>.<name>"
        const ModHost::ModParam *param;
        wxWindow               *control;
        std::vector<wxWindow *> windows; // label, control and unit, shown and hidden together
    };

    void commit();
    // The value a row's control shows, as the parameter's type.
    std::optional<ModHost::ExprValue> row_value(const Row &row) const;

    std::shared_ptr<const ModHost::ModSet> m_mods;
    std::string                            m_scope;
    std::vector<Row>                       m_rows;
    // The stored object as last loaded, so values of mods that are not loaded survive an edit.
    std::string                            m_loaded_json;
    bool                                   m_loading = false;
};

} // namespace GUI
} // namespace Slic3r
