#pragma once

#include <libslic3r/ModHost/ModSet.hpp>

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class wxStaticText;
class wxWindow;
class ScalableButton;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

class ModLayerRangesEditor;

// Rows of mod parameters a settings tab stores in its preset (process, printer or filament): the tab's Mods page,
// or the parameters a mod places next to Orca's own settings (ModParamUi). Edits go into the preset's mod value key
// (mod_settings, printer_mod_settings or filament_mod_settings) as JSON, so the preset is dirtied and saved like any
// other setting.
class ModParamsPanel : public wxPanel
{
public:
    // `scope` is "process", "printer" or "filament". `keys` ("<mod id>.<name>") are the rows, in their mods' order.
    // Without labels the panel holds a single row whose label the tab's line draws.
    ModParamsPanel(wxWindow *parent, std::shared_ptr<const ModHost::ModSet> mods, const std::string &scope,
                   const std::vector<std::string> &keys, bool labels = true);

    // The parameters the loaded mods store in `scope`'s preset, as "<mod id>.<name>", in their mods' order.
    static std::vector<std::string> param_keys(const ModHost::ModSet *mods, const std::string &scope);
    // The parameter "<mod id>.<name>" of `mods`, or nullptr.
    static const ModHost::ModParam *find_param(const ModHost::ModSet *mods, const std::string &key);
    // The label a parameter's row shows, in the application's language.
    static wxString param_label(const ModHost::ModSet *mods, const std::string &key);
    // The config key that stores the values of `scope`.
    static const char *config_key(const std::string &scope);
    // The JSON text `config` stores for `scope` (empty when it stores none).
    static std::string stored_json(const DynamicPrintConfig &config, const std::string &scope);

    // Shows the values stored in `config` (defaults where none is stored).
    void load_from_config(const DynamicPrintConfig &config);
    // Called with the new JSON text after the user changes a value.
    std::function<void(const std::string &json_text)> on_change;
    // The JSON the preset holds now. A tab with several panels on one key sets it, so a panel's edit starts from what
    // the others wrote and never puts back a value they changed since it last loaded.
    std::function<std::string()> current_text;
    // Shows and hides rows by their "visible" conditions. A condition may read a parameter another
    // tab stores, so the tabs call this on each other after a change.
    void update_visibility();

    // The JSON the preset held when last saved. A row whose value differs from it shows Orca's modified label colour
    // and a revert button, which puts the saved value back.
    std::function<std::string()> saved_text;
    // Called when the modified state of the panel's rows changes (any row differs from saved_text), for a label the
    // tab's line draws (a single row without labels).
    std::function<void(bool modified)> on_modified;
    // Called with whether any row shows, after rows were shown or hidden by their conditions, so a group holding only
    // this panel can hide with it.
    std::function<void(bool any_shown)> on_rows_shown;
    // Shows each row's modified state against saved_text.
    void update_modified();
    // For a single row without labels: how far its control sits right of the panel's left edge (the revert button and
    // its gap), so the settings page can place the control in the value column.
    int value_column_offset() const;

private:
    struct Row
    {
        std::string            key; // "<mod id>.<name>"
        const ModHost::ModParam *param;
        wxWindow               *control;
        std::vector<wxWindow *> windows; // label, revert button, control and unit, shown and hidden together
        wxStaticText           *label  = nullptr; // nullptr without labels
        ScalableButton         *revert = nullptr;
        bool                    modified = false;
        // A dropdown's values, in its order: the parameter's options, or the implementations its choices_from
        // slot lists (ModHost::support_choices), or a toolhead number.
        std::vector<std::string> choice_values;
    };

    void commit();
    // Shows the values of the stored JSON `text`.
    void load_text(const std::string &text);
    // Puts back the value the saved preset holds for the row `key` (the revert button).
    void revert(const std::string &key);
    // The saved preset's own text when `text` holds the same values (defaults filled in for any loaded mod's), else
    // `text`: rewriting the same values must not leave the preset marked modified.
    std::string kept_saved_text(const std::string &text) const;
    // The value a row's control shows, as the parameter's type.
    std::optional<ModHost::ExprValue> row_value(const Row &row) const;
    // The toolhead dropdown's entries for the printer being edited: "Auto", then one per nozzle.
    void fill_toolhead_choice(Row &row);

    std::shared_ptr<const ModHost::ModSet> m_mods;
    std::string                            m_scope;
    std::vector<Row>                       m_rows;
    // The stored object as last loaded, so values of mods that are not loaded survive an edit.
    std::string                            m_loaded_json;
    bool                                   m_loading = false;
    bool                                   m_any_modified = false;
    std::optional<bool>                    m_any_shown;
};

} // namespace GUI
} // namespace Slic3r
