#ifndef slic3r_GLGizmoFdmSupports_hpp_
#define slic3r_GLGizmoFdmSupports_hpp_

#include "GLGizmoPainterBase.hpp"
//BBS
#include "libslic3r/Print.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/thread.hpp>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include <string>
#include <wx/string.h>
#include <cstddef>
#include <vector>
#include <mutex>
#include <map>
#include <utility>

namespace Slic3r::GUI {

class GLGizmoFdmSupports : public GLGizmoPainterBase
{
public:
    GLGizmoFdmSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    //BBS: add edit state
    enum EditState {
        state_idle = 0,
        state_generating = 1,
        state_ready
    };

    bool on_tool_shortcut(Shortcut shortcut) override;

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    void on_set_state() override;
    void render_tooltip_button(float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Paint-on supports"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Paint-on supports"); }
    std::string get_action_snapshot_name() const override { return _u8L("Paint-on supports editing"); }

    // BBS
    wchar_t                           m_current_tool = 0;

private:
    bool on_init() override;

    //BBS: remove const
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void select_facets_by_angle(float threshold, bool block);
    // BBS
    int get_selection_support_threshold_angle();

    int m_support_threshold_angle = -1;

    //BBS: add support preview logic
    void init_print_instance();
    void update_support_volumes();
    void invalid_support_volumes(bool invalid_step = false);
    bool need_regenerate_support_volumes();
    void generate_support_volume();
    void run_thread();
    float m_angle_threshold_deg = 40.f;
    bool m_volume_valid = false;


    GLVolume *m_support_volume = NULL;
    mutable bool m_volume_ready = false;
    bool m_is_tree_support = false;
    bool m_cancel = false;
    size_t m_object_id;
    std::vector<ObjectBase::Timestamp> m_volume_timestamps;

    // MAGPIE: while a mod's support paint channels apply (its manifest's gui paint_channels and paint_when,
    // e.g. Mixed support's normal and tree channels), the left button paints a channel as well. The
    // painting keeps the states below in one selector and splits them into supported_facets and
    // support_channel_facets (channel 0 as ENFORCER, channel 1 as BLOCKER) when it is stored.
    EnforcerBlockerType get_left_button_state_type() const override;
    struct PaintChannel
    {
        wxString  label;
        ColorRGBA color;
    };
    // Whether the selectors were built for channel painting: read once in update_from_model_object, so
    // painting, colours and storing always agree with the selectors (and no config copy per call).
    bool                      is_mixed_mode() const { return !m_paint_channels.empty(); }
    std::vector<PaintChannel> read_paint_channels() const;
    std::vector<PaintChannel> m_paint_channels;
    const PaintChannel       &current_channel() const { return m_paint_channels[m_mixed_paint_state == MixedTreeState && m_paint_channels.size() > 1 ? 1 : 0]; }
    static constexpr EnforcerBlockerType MixedNormalState = EnforcerBlockerType::Extruder3;
    static constexpr EnforcerBlockerType MixedTreeState   = EnforcerBlockerType::Extruder4;
    EnforcerBlockerType                  m_mixed_paint_state = MixedNormalState;
    std::vector<ObjectBase::Timestamp>   m_channel_timestamps;
    PrintInstance m_print_instance;
    mutable EditState m_edit_state;
    //thread
    boost::thread   m_thread;
    // Mutex and condition variable to synchronize m_thread with the UI thread.
    std::mutex      m_mutex;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;

    // Contains all shortcuts in the format of {shortcut, description}, e.g. {alt + _L("Left mouse button"), _L("Part_selection")}
    std::vector<std::pair<wxString, wxString>> m_shortcuts_brush;
    // Contains all shortcuts in the format of {shortcut, description}, e.g. {alt + _L("Left mouse button"), _L("Part_selection")}
    std::vector<std::pair<wxString, wxString>> m_shortcuts_bucket_fill;
    // Contains all shortcuts in the format of {shortcut, description}, e.g. {alt + _L("Left mouse button"), _L("Part_selection")}
    std::vector<std::pair<wxString, wxString>> m_shortcuts_gap_fill;
};



} // namespace Slic3r::GUI


#endif // slic3r_GLGizmoFdmSupports_hpp_
