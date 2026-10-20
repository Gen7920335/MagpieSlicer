#pragma once

#include <libslic3r/ModHost/ModSet.hpp>

#include <wx/dialog.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class wxCheckBox;
class wxWindow;

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace GUI {

// MAGPIE: "Mod settings" of one object, part or modifier (declarative GUI, docs/HLSD/mods.md). Lists the
// object- and region-scope parameters of the loaded mods (an object shows both, a part or modifier
// its region parameters), in the order the mod's "gui"."object_params" gives. A row set here
// overrides the value from the level above; an unset row shows the value it inherits. OK writes the
// values as JSON to the object's object_mod_settings / region_mod_settings or the volume's
// region_mod_settings, with an undo snapshot, and reslices.
class ModObjectSettingsDialog : public wxDialog
{
public:
    // `volume` null edits the object itself.
    ModObjectSettingsDialog(wxWindow *parent, ModelObject &object, ModelVolume *volume);

    // Whether the loaded mods have a parameter this dialog would show for an object (volume false)
    // or a part or modifier (volume true).
    static bool has_params(bool volume);

private:
    struct Row
    {
        std::string              key;    // "<mod id>.<name>"
        const ModHost::ModParam *param;
        std::string              store;  // "object_mod_settings" or "region_mod_settings"
        wxCheckBox              *set_here;
        wxWindow                *control;
    };

    void apply();
    void show_value(const Row &row, const ModHost::ExprValue &value);
    // The control's value as the parameter's type, or nothing when it is not one.
    std::optional<ModHost::ExprValue> control_value(const Row &row) const;
    // The value a row has without a value of its own at this level.
    ModHost::ExprValue inherited_value(const Row &row) const;

    std::shared_ptr<const ModHost::ModSet> m_mods;
    ModelObject                           &m_object;
    ModelVolume                           *m_volume;
    std::vector<Row>                       m_rows;
};

} // namespace GUI
} // namespace Slic3r
