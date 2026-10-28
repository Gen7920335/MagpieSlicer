#ifndef slic3r_ModCalibrationDialog_hpp_
#define slic3r_ModCalibrationDialog_hpp_

// MAGPIE: the form of a calibration a mod declares ("gui"."calibrations"[]."form", ModSet.hpp), drawn as Orca's own
// calibration dialogs are: numbers with their units, dropdowns, settings of the presets being edited (with the choices
// mods merge into them, MergedSettings.hpp) and groups the user folds open. While the user edits it the mod checks the
// values (calibration.project slot); OK starts the calibration's project (Plater::calib_mod). Nothing in it knows a
// calibration.

#include "GUI_Utils.hpp"

#include "libslic3r/ModHost/CalibrationProject.hpp"
#include "libslic3r/ModHost/ModSet.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class wxBoxSizer;
class wxStaticText;

namespace Slic3r {
namespace GUI {

class Plater;

class ModCalibrationDialog : public DPIDialog
{
public:
    ModCalibrationDialog(wxWindow *parent, Plater *plater, const ModHost::Mod &mod, const ModHost::ModGuiItem &calibration);
    ~ModCalibrationDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Field;
    struct Group;

    // Every field's value, hidden ones included.
    ModHost::CalibrationFields fields() const;
    // What the visible setting fields write into the presets: (key, serialized value or a merged parameter's JSON).
    std::vector<std::pair<std::string, std::string>> setting_writes() const;
    void on_changed();
    void on_ok();

    Plater                    *m_plater = nullptr;
    std::string                m_mod_id;
    ModHost::ModGuiItem        m_calibration;
    std::vector<std::unique_ptr<Field>> m_fields;
    std::vector<std::unique_ptr<Group>> m_groups;
    wxStaticText              *m_summary  = nullptr;
    wxStaticText              *m_warnings = nullptr;
    wxStaticText              *m_errors   = nullptr;
    wxWindow                  *m_ok       = nullptr;
    bool                       m_updating = false;
};

} // namespace GUI
} // namespace Slic3r

#endif
