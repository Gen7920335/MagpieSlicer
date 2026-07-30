#include "SnapmakerMonitorPanel.hpp"
#include "SnapmakerCameraSession.hpp"
#include "SnapmakerGCodeLayer.hpp"
#include "SnapmakerMonitorUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/GCode/SnapmakerHomingPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include <wx/bitmap.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>

namespace Slic3r {
namespace GUI {

class CameraCanvas final : public wxPanel
{
public:
    explicit CameraCanvas(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(wxColour(12, 16, 21));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &CameraCanvas::on_paint, this);
        Bind(wxEVT_SIZE, &CameraCanvas::on_size, this);
    }

    void set_frame(wxImage image)
    {
        if (!image.IsOk())
            return;
        m_frame = std::move(image);
        rebuild_bitmap(true);
        Refresh(false);
    }

private:
    void rebuild_bitmap(bool force = false)
    {
        if (!m_frame.IsOk())
            return;
        const wxSize client = GetClientSize();
        if (client.x <= 0 || client.y <= 0)
            return;

        const double scale = std::min(
            static_cast<double>(client.x) / m_frame.GetWidth(),
            static_cast<double>(client.y) / m_frame.GetHeight());
        const int width = std::max(1, static_cast<int>(std::lround(m_frame.GetWidth() * scale)));
        const int height = std::max(1, static_cast<int>(std::lround(m_frame.GetHeight() * scale)));
        if (!force && m_bitmap.IsOk() && m_bitmap.GetWidth() == width && m_bitmap.GetHeight() == height)
            return;
        m_bitmap = wxBitmap(m_frame.Scale(width, height, wxIMAGE_QUALITY_BILINEAR));
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        if (!m_bitmap.IsOk())
            return;
        const wxSize client = GetClientSize();
        const int x = std::max(0, (client.x - m_bitmap.GetWidth()) / 2);
        const int y = std::max(0, (client.y - m_bitmap.GetHeight()) / 2);
        dc.DrawBitmap(m_bitmap, x, y, false);
    }

    void on_size(wxSizeEvent &event)
    {
        rebuild_bitmap();
        Refresh(false);
        event.Skip();
    }

    wxImage m_frame;
    wxBitmap m_bitmap;
};

class TemperatureHistoryCanvas final : public wxPanel
{
public:
    explicit TemperatureHistoryCanvas(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(wxColour(12, 16, 21));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &TemperatureHistoryCanvas::on_paint, this);
    }

    void add_sample(const std::array<double, 5> &temperatures)
    {
        const auto now = std::chrono::steady_clock::now();
        m_samples.push_back({now, temperatures});
        const auto cutoff = now - std::chrono::minutes(10);
        while (!m_samples.empty() && m_samples.front().time < cutoff)
            m_samples.pop_front();
        Refresh(false);
    }

    void clear()
    {
        m_samples.clear();
        Refresh(false);
    }

private:
    struct Sample
    {
        std::chrono::steady_clock::time_point time;
        std::array<double, 5> temperatures;
    };

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        static const std::array<wxColour, 5> colours {
            wxColour(61, 145, 232),
            wxColour(239, 166, 70),
            wxColour(216, 92, 156),
            wxColour(70, 196, 185),
            wxColour(225, 102, 90)
        };
        static const std::array<const char *, 5> names {"T1", "T2", "T3", "T4", "Bed"};

        const wxSize size = GetClientSize();
        const int left = FromDIP(42);
        const int right = FromDIP(12);
        const int top = FromDIP(42);
        const int bottom = FromDIP(24);
        const int width = std::max(1, size.x - left - right);
        const int height = std::max(1, size.y - top - bottom);

        dc.SetTextForeground(wxColour(220, 228, 237));
        wxFont title_font = GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(title_font);
        dc.DrawText(_L("Temperature history") + " (10 min)", FromDIP(12), FromDIP(8));

        dc.SetFont(GetFont());
        int legend_x = left;
        for (size_t i = 0; i < names.size(); ++i) {
            dc.SetPen(wxPen(colours[i], 3));
            dc.DrawLine(legend_x, FromDIP(31), legend_x + FromDIP(12), FromDIP(31));
            dc.SetTextForeground(wxColour(182, 194, 207));
            dc.DrawText(names[i], legend_x + FromDIP(16), FromDIP(23));
            legend_x += FromDIP(i == names.size() - 1 ? 54 : 45);
        }

        double min_temperature = std::numeric_limits<double>::max();
        double max_temperature = std::numeric_limits<double>::lowest();
        for (const Sample &sample : m_samples) {
            for (double temperature : sample.temperatures) {
                if (!std::isfinite(temperature))
                    continue;
                min_temperature = std::min(min_temperature, temperature);
                max_temperature = std::max(max_temperature, temperature);
            }
        }
        if (min_temperature > max_temperature) {
            min_temperature = 0.0;
            max_temperature = 100.0;
        } else {
            min_temperature = std::max(0.0, std::floor((min_temperature - 10.0) / 10.0) * 10.0);
            max_temperature = std::ceil((max_temperature + 10.0) / 10.0) * 10.0;
            if (max_temperature - min_temperature < 50.0)
                max_temperature = min_temperature + 50.0;
        }

        dc.SetPen(wxPen(wxColour(55, 67, 81), 1));
        for (int row = 0; row <= 4; ++row) {
            const int y = top + height * row / 4;
            dc.DrawLine(left, y, left + width, y);
            const double value = max_temperature -
                (max_temperature - min_temperature) * static_cast<double>(row) / 4.0;
            dc.SetTextForeground(wxColour(137, 151, 166));
            dc.DrawText(wxString::Format("%.0f", value), FromDIP(5), y - FromDIP(7));
        }
        dc.SetTextForeground(wxColour(137, 151, 166));
        dc.DrawText("-10m", left, top + height + FromDIP(4));
        dc.DrawText(_L("Now"), left + width - FromDIP(28), top + height + FromDIP(4));

        if (m_samples.size() < 2)
            return;
        const auto now = m_samples.back().time;
        const double range = max_temperature - min_temperature;
        for (size_t series = 0; series < names.size(); ++series) {
            dc.SetPen(wxPen(colours[series], 2));
            bool has_previous = false;
            wxPoint previous;
            for (const Sample &sample : m_samples) {
                const double temperature = sample.temperatures[series];
                if (!std::isfinite(temperature)) {
                    has_previous = false;
                    continue;
                }
                const double age = std::chrono::duration<double>(now - sample.time).count();
                const int x = left + static_cast<int>(std::lround(width * std::clamp(1.0 - age / 600.0, 0.0, 1.0)));
                const int y = top + static_cast<int>(std::lround(
                    height * (max_temperature - temperature) / range));
                const wxPoint point(x, y);
                if (has_previous)
                    dc.DrawLine(previous, point);
                previous = point;
                has_previous = true;
            }
        }
    }

    std::deque<Sample> m_samples;
};

struct ExcludeObjectShape
{
    std::string name;
    std::vector<SnapmakerGCodePoint> polygon;
    bool excluded {false};
};

class LayerToolpathCanvas final : public wxPanel
{
public:
    explicit LayerToolpathCanvas(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(wxColour(12, 16, 21));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &LayerToolpathCanvas::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &LayerToolpathCanvas::on_left_down, this);
    }

    void set_selection_callback(std::function<void(const std::string &)> callback)
    {
        m_selection_callback = std::move(callback);
    }

    void set_status(const wxString &status)
    {
        m_status = status;
        m_layer = {};
        m_current_layer = 0;
        m_total_layers = 0;
        Refresh(false);
    }

    void set_layer(SnapmakerGCodeLayer layer, int total_layers)
    {
        m_current_layer = layer.number;
        m_total_layers = total_layers;
        m_layer = std::move(layer);
        m_status.clear();
        Refresh(false);
    }

    void set_objects(std::vector<ExcludeObjectShape> objects)
    {
        m_objects = std::move(objects);
        const auto selected = std::find_if(m_objects.begin(), m_objects.end(), [this](const ExcludeObjectShape &object) {
            return object.name == m_selected_object && !object.excluded;
        });
        if (!m_selected_object.empty() && selected == m_objects.end())
            set_selected_object({});
        Refresh(false);
    }

    void set_bed_bounds(double min_x, double min_y, double max_x, double max_y)
    {
        if (max_x <= min_x || max_y <= min_y)
            return;
        m_bed_min = {min_x, min_y};
        m_bed_max = {max_x, max_y};
        m_has_bed_bounds = true;
        Refresh(false);
    }

    const std::string &selected_object() const
    {
        return m_selected_object;
    }

    void select_object(const std::string &name)
    {
        const auto object = std::find_if(m_objects.begin(), m_objects.end(), [&name](const ExcludeObjectShape &candidate) {
            return candidate.name == name && !candidate.excluded;
        });
        set_selected_object(object == m_objects.end() ? std::string() : name);
    }

private:
    struct ViewTransform
    {
        double min_x {0.0};
        double min_y {0.0};
        double scale {0.0};
        double left {0.0};
        double bottom {0.0};

        wxPoint screen(const SnapmakerGCodePoint &point) const
        {
            return {
                static_cast<int>(std::lround(left + (point.x - min_x) * scale)),
                static_cast<int>(std::lround(bottom - (point.y - min_y) * scale))
            };
        }

        SnapmakerGCodePoint model(const wxPoint &point) const
        {
            if (scale <= 0.0)
                return {};
            return {
                min_x + (point.x - left) / scale,
                min_y + (bottom - point.y) / scale
            };
        }
    };

    static bool point_in_polygon(const SnapmakerGCodePoint &point, const std::vector<SnapmakerGCodePoint> &polygon)
    {
        if (polygon.size() < 3)
            return false;
        bool inside = false;
        for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            const auto &a = polygon[i];
            const auto &b = polygon[j];
            const bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
                (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x);
            if (crosses)
                inside = !inside;
        }
        return inside;
    }

    void set_selected_object(std::string name)
    {
        if (m_selected_object == name)
            return;
        m_selected_object = std::move(name);
        if (m_selection_callback)
            m_selection_callback(m_selected_object);
        Refresh(false);
    }

    void on_left_down(wxMouseEvent &event)
    {
        if (m_view.scale <= 0.0)
            return;
        const SnapmakerGCodePoint model_point = m_view.model(event.GetPosition());
        const ExcludeObjectShape *hit = nullptr;
        double hit_area = std::numeric_limits<double>::max();
        for (const ExcludeObjectShape &object : m_objects) {
            if (object.excluded || !point_in_polygon(model_point, object.polygon))
                continue;
            double min_x = std::numeric_limits<double>::max();
            double max_x = std::numeric_limits<double>::lowest();
            double min_y = std::numeric_limits<double>::max();
            double max_y = std::numeric_limits<double>::lowest();
            for (const auto &point : object.polygon) {
                min_x = std::min(min_x, point.x);
                max_x = std::max(max_x, point.x);
                min_y = std::min(min_y, point.y);
                max_y = std::max(max_y, point.y);
            }
            const double area = (max_x - min_x) * (max_y - min_y);
            if (area < hit_area) {
                hit = &object;
                hit_area = area;
            }
        }
        set_selected_object(hit == nullptr || hit->name == m_selected_object ? std::string() : hit->name);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxSize size = GetClientSize();
        const int padding = FromDIP(14);
        const int title_height = FromDIP(30);
        dc.SetTextForeground(wxColour(220, 228, 237));
        wxFont title_font = GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(title_font);
        const wxString title = m_current_layer > 0
            ? wxString::Format("%s %d / %d", _L("G-code layer"), m_current_layer, m_total_layers)
            : _L("G-code layer");
        dc.DrawText(title, padding, FromDIP(8));

        double min_x = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double min_y = std::numeric_limits<double>::max();
        double max_y = std::numeric_limits<double>::lowest();
        auto include = [&](const SnapmakerGCodePoint &point) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        };
        if (m_has_bed_bounds) {
            include(m_bed_min);
            include(m_bed_max);
        } else {
            for (const ExcludeObjectShape &object : m_objects)
                for (const auto &point : object.polygon)
                    include(point);
            for (const SnapmakerGCodeSegment &segment : m_layer.segments) {
                include(segment.from);
                include(segment.to);
            }
        }

        const int draw_width = std::max(0, size.x - padding * 2);
        const int draw_height = std::max(0, size.y - title_height - padding * 2);
        if (min_x > max_x || min_y > max_y || draw_width == 0 || draw_height == 0) {
            dc.SetFont(GetFont());
            dc.SetTextForeground(wxColour(150, 164, 179));
            dc.DrawText(
                m_status.empty() ? _L("No extrusion paths for this layer") : m_status,
                padding,
                title_height + padding);
            m_view = {};
            return;
        }

        const double range_x = std::max(1.0, max_x - min_x);
        const double range_y = std::max(1.0, max_y - min_y);
        const double scale = std::min(draw_width / range_x, draw_height / range_y);
        const double content_width = range_x * scale;
        const double content_height = range_y * scale;
        m_view = {
            min_x,
            min_y,
            scale,
            padding + (draw_width - content_width) * 0.5,
            title_height + padding + (draw_height + content_height) * 0.5
        };

        if (m_has_bed_bounds) {
            const wxPoint top_left = m_view.screen({m_bed_min.x, m_bed_max.y});
            const wxPoint bottom_right = m_view.screen({m_bed_max.x, m_bed_min.y});
            dc.SetPen(wxPen(wxColour(76, 91, 108), 2));
            dc.SetBrush(wxBrush(wxColour(17, 23, 30)));
            dc.DrawRectangle(
                top_left.x,
                top_left.y,
                std::max(1, bottom_right.x - top_left.x),
                std::max(1, bottom_right.y - top_left.y));
        }

        for (const ExcludeObjectShape &object : m_objects) {
            if (object.polygon.size() < 3)
                continue;
            std::vector<wxPoint> points;
            points.reserve(object.polygon.size());
            for (const auto &point : object.polygon)
                points.push_back(m_view.screen(point));
            const bool selected = object.name == m_selected_object;
            const wxColour outline = object.excluded
                ? wxColour(110, 119, 130)
                : selected ? wxColour(61, 145, 232) : wxColour(73, 88, 104);
            const wxColour fill = object.excluded
                ? wxColour(70, 76, 84)
                : selected ? wxColour(35, 73, 112) : wxColour(18, 24, 31);
            dc.SetPen(wxPen(outline, selected ? 3 : 1));
            dc.SetBrush(wxBrush(fill));
            dc.DrawPolygon(static_cast<int>(points.size()), points.data());
        }

        static const std::array<wxColour, 8> tool_colours {
            wxColour(61, 145, 232),
            wxColour(239, 166, 70),
            wxColour(216, 92, 156),
            wxColour(70, 196, 185),
            wxColour(151, 113, 221),
            wxColour(225, 102, 90),
            wxColour(132, 188, 89),
            wxColour(93, 172, 220)
        };
        const std::unordered_set<std::string> excluded = [&]() {
            std::unordered_set<std::string> names;
            for (const auto &object : m_objects)
                if (object.excluded)
                    names.insert(object.name);
            return names;
        }();
        for (const SnapmakerGCodeSegment &segment : m_layer.segments) {
            const bool is_excluded = excluded.find(segment.object) != excluded.end();
            const bool selected = !m_selected_object.empty() && segment.object == m_selected_object;
            const wxColour colour = is_excluded
                ? wxColour(104, 112, 122)
                : tool_colours[segment.tool % tool_colours.size()];
            dc.SetPen(wxPen(colour, selected ? 3 : 2));
            dc.DrawLine(m_view.screen(segment.from), m_view.screen(segment.to));
        }
    }

    SnapmakerGCodeLayer m_layer;
    std::vector<ExcludeObjectShape> m_objects;
    std::string m_selected_object;
    std::function<void(const std::string &)> m_selection_callback;
    wxString m_status {_L("Waiting for G-code")};
    int m_current_layer {0};
    int m_total_layers {0};
    bool m_has_bed_bounds {false};
    SnapmakerGCodePoint m_bed_min;
    SnapmakerGCodePoint m_bed_max;
    ViewTransform m_view;
};

namespace {

using json = nlohmann::json;

constexpr int POLL_INTERVAL_MS = 1000;
constexpr int CAMERA_INTERVAL_MS = 150;
constexpr size_t MAX_GCODE_BYTES = 512ULL * 1024ULL * 1024ULL;
const wxColour ACCENT_BLUE(61, 145, 232);

std::uint64_t frame_hash(const std::string &body)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : body) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

double number_or(const json &value, const char *key, double fallback = 0.0)
{
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number())
        return fallback;
    return it->get<double>();
}

std::string string_or(const json &value, const char *key)
{
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
}

wxString temperature_text(const wxString &name, const json &status)
{
    const double current = number_or(status, "temperature");
    const double target  = number_or(status, "target");
    return wxString::Format("%s %.0f/%.0f C", name, current, target);
}

wxString duration_text(double seconds)
{
    const auto total = static_cast<long long>(std::max(0.0, seconds));
    const auto hours = total / 3600;
    const auto mins  = (total % 3600) / 60;
    const auto secs  = total % 60;
    return wxString::Format("%02lld:%02lld:%02lld", hours, mins, secs);
}

wxStaticText *add_value_row(wxWindow *parent, wxSizer *sizer, const wxString &label)
{
    auto *value = new wxStaticText(parent, wxID_ANY, label);
    value->SetMinSize(parent->FromDIP(wxSize(-1, 24)));
    sizer->Add(value, wxSizerFlags().Expand().Border(wxBOTTOM, parent->FromDIP(5)));
    return value;
}

wxStaticText *add_section_title(wxWindow *parent, wxSizer *sizer, const wxString &label)
{
    auto *title = new wxStaticText(parent, wxID_ANY, label);
    wxFont font = title->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(font);
    sizer->Add(title, wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, parent->FromDIP(8)));
    return title;
}

wxString display_state(const std::string &state)
{
    if (state == "printing")
        return _L("Printing");
    if (state == "paused")
        return _L("Paused");
    if (state == "complete")
        return _L("Complete");
    if (state == "cancelled")
        return _L("Cancelled");
    if (state == "error")
        return _L("Error");
    if (state == "standby" || state.empty())
        return _L("Ready");
    return from_u8(state);
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string uppercase_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string encode_url_path(const std::string &path)
{
    std::string encoded;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t separator = path.find('/', start);
        if (!encoded.empty())
            encoded += '/';
        encoded += Slic3r::Http::url_encode(path.substr(
            start,
            separator == std::string::npos ? std::string::npos : separator - start));
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    return encoded;
}

bool is_extruder_object(const std::string &name)
{
    if (name == "extruder")
        return true;
    const std::string prefix = "extruder";
    return starts_with(name, prefix) && name.size() > prefix.size() &&
           std::all_of(name.begin() + prefix.size(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace

SnapmakerMonitorPanel::SnapmakerMonitorPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
    , m_status_timer(this)
    , m_camera_timer(this)
    , m_camera_interval_ms(CAMERA_INTERVAL_MS)
    , m_fps_window_started(std::chrono::steady_clock::now())
    , m_lifetime(std::make_shared<int>(0))
    , m_camera_session(std::make_unique<SnapmakerCameraSession>())
{
    build_ui();
    Bind(wxEVT_TIMER, &SnapmakerMonitorPanel::on_status_timer, this, m_status_timer.GetId());
    Bind(wxEVT_TIMER, &SnapmakerMonitorPanel::on_camera_timer, this, m_camera_timer.GetId());
}

SnapmakerMonitorPanel::~SnapmakerMonitorPanel()
{
    m_status_timer.Stop();
    m_camera_timer.Stop();
    stop_camera_session();
    m_lifetime.reset();
}

void SnapmakerMonitorPanel::build_ui()
{
    const bool dark = wxSystemSettings::GetAppearance().IsDark();
    const wxColour background = dark ? wxColour(22, 28, 36) : wxColour(242, 245, 249);
    const wxColour panel_bg   = dark ? wxColour(31, 39, 49) : *wxWHITE;
    const wxColour panel_alt  = dark ? wxColour(42, 51, 63) : wxColour(235, 240, 246);
    const wxColour muted      = dark ? wxColour(164, 177, 191) : wxColour(86, 99, 113);

    SetBackgroundColour(background);

    auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(background);
    scroll->SetScrollRate(0, FromDIP(12));
    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *header = new wxPanel(scroll);
    header->SetBackgroundColour(panel_bg);
    header->SetMinSize(FromDIP(wxSize(-1, 58)));
    auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new wxStaticText(header, wxID_ANY, _L("Snapmaker U1"));
    wxFont title_font = title->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 4);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    header_sizer->Add(title, wxSizerFlags().CenterVertical().Border(wxLEFT, FromDIP(18)));

    m_connection = new wxStaticText(header, wxID_ANY, _L("Connecting..."));
    m_connection->SetForegroundColour(muted);
    header_sizer->Add(m_connection, wxSizerFlags().CenterVertical().Border(wxLEFT, FromDIP(16)));
    header_sizer->AddStretchSpacer();

    m_state = new wxStaticText(header, wxID_ANY, _L("Unavailable"));
    wxFont state_font = m_state->GetFont();
    state_font.SetPointSize(state_font.GetPointSize() + 2);
    state_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_state->SetFont(state_font);
    m_state->SetForegroundColour(ACCENT_BLUE);
    m_state->SetMinSize(FromDIP(wxSize(120, -1)));
    header_sizer->Add(m_state, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(12)));

    m_progress_percent = new wxStaticText(header, wxID_ANY, "0.0%");
    wxFont percent_font = m_progress_percent->GetFont();
    percent_font.SetPointSize(percent_font.GetPointSize() + 2);
    percent_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_progress_percent->SetFont(percent_font);
    m_progress_percent->SetForegroundColour(ACCENT_BLUE);
    m_progress_percent->SetMinSize(FromDIP(wxSize(72, -1)));
    header_sizer->Add(m_progress_percent, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(18)));
    header->SetSizer(header_sizer);
    root->Add(header, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP, FromDIP(12)));

    auto *main_area = new wxBoxSizer(wxHORIZONTAL);

    auto *camera_panel = new wxPanel(scroll);
    camera_panel->SetBackgroundColour(background);
    auto *camera_sizer = new wxBoxSizer(wxVERTICAL);
    m_camera = new CameraCanvas(camera_panel);
    m_camera->SetMinSize(FromDIP(wxSize(520, 293)));
    m_camera->SetMaxSize(FromDIP(wxSize(-1, 293)));
    camera_sizer->Add(m_camera, wxSizerFlags().Expand());
    m_temperature_history = new TemperatureHistoryCanvas(camera_panel);
    m_temperature_history->SetMinSize(FromDIP(wxSize(520, 220)));
    camera_sizer->Add(
        m_temperature_history,
        wxSizerFlags().Expand().Proportion(1).Border(wxTOP, FromDIP(10)));
    camera_panel->SetSizer(camera_sizer);
    camera_panel->Bind(wxEVT_SIZE, [this, camera_panel](wxSizeEvent &event) {
        const int width = std::max(1, event.GetSize().x);
        const int camera_height = std::max(1, static_cast<int>(std::lround(width * 9.0 / 16.0)));
        const wxSize current = m_camera->GetMinSize();
        if (current.y != camera_height) {
            m_camera->SetMinSize(wxSize(-1, camera_height));
            m_camera->SetMaxSize(wxSize(-1, camera_height));
            camera_panel->Layout();
        }
        event.Skip();
    });
    main_area->Add(camera_panel, wxSizerFlags().Expand().Proportion(9).Border(wxRIGHT, FromDIP(12)));

    auto *layer_panel = new wxPanel(scroll);
    layer_panel->SetBackgroundColour(background);
    auto *layer_sizer = new wxBoxSizer(wxVERTICAL);
    m_layer_view = new LayerToolpathCanvas(layer_panel);
    m_layer_view->SetMinSize(FromDIP(wxSize(390, 420)));
    m_layer_view->set_selection_callback([this](const std::string &name) {
        m_selected_object->SetLabel(name.empty()
            ? _L("Select a model from the layer view")
            : _L("Selected") + ": " + from_u8(name));
        m_selected_object->SetToolTip(name.empty() ? wxString() : from_u8(name));
        if (m_object_list != nullptr) {
            const auto selected = std::find(m_object_names.begin(), m_object_names.end(), name);
            m_object_list->SetSelection(
                selected == m_object_names.end()
                    ? wxNOT_FOUND
                    : static_cast<int>(std::distance(m_object_names.begin(), selected)));
        }
        update_controls();
    });
    layer_sizer->Add(m_layer_view, wxSizerFlags().Expand().Proportion(1));

    auto *object_controls = new wxBoxSizer(wxHORIZONTAL);
    m_selected_object = new wxStaticText(layer_panel, wxID_ANY, _L("Select a model from the layer view"));
    m_selected_object->SetForegroundColour(muted);
    object_controls->Add(m_selected_object, wxSizerFlags().CenterVertical().Proportion(1).Border(wxLEFT, FromDIP(4)));
    m_skip_object = new Button(layer_panel, _L("Skip selected model"), "media_stop", 0, 14);
    m_skip_object->SetStyle(ButtonStyle::Alert, ButtonType::Window);
    m_skip_object->SetMinSize(FromDIP(wxSize(152, 36)));
    m_skip_object->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_skip_object(); });
    object_controls->Add(m_skip_object, wxSizerFlags().CenterVertical().Border(wxLEFT, FromDIP(8)));
    layer_sizer->Add(object_controls, wxSizerFlags().Expand().Border(wxTOP, FromDIP(8)));

    auto *models_title = new wxStaticText(layer_panel, wxID_ANY, _L("Models"));
    wxFont models_font = models_title->GetFont();
    models_font.SetWeight(wxFONTWEIGHT_BOLD);
    models_title->SetFont(models_font);
    models_title->SetForegroundColour(muted);
    layer_sizer->Add(models_title, wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, FromDIP(8)));
    m_object_list = new wxListBox(layer_panel, wxID_ANY);
    m_object_list->SetMinSize(FromDIP(wxSize(-1, 112)));
    m_object_list->SetBackgroundColour(wxColour(17, 23, 30));
    m_object_list->SetForegroundColour(wxColour(220, 228, 237));
    m_object_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        const int selection = m_object_list->GetSelection();
        if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_object_names.size())
            m_layer_view->select_object({});
        else
            m_layer_view->select_object(m_object_names[static_cast<size_t>(selection)]);
    });
    layer_sizer->Add(m_object_list, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(1)));
    layer_panel->SetSizer(layer_sizer);
    main_area->Add(layer_panel, wxSizerFlags().Expand().Proportion(6).Border(wxRIGHT, FromDIP(12)));

    auto *details = new wxPanel(scroll);
    details->SetBackgroundColour(panel_bg);
    details->SetMinSize(FromDIP(wxSize(280, -1)));
    auto *detail_sizer = new wxBoxSizer(wxVERTICAL);

    auto *job_title = add_section_title(details, detail_sizer, _L("Current print"));
    job_title->SetForegroundColour(muted);
    m_filename = new wxStaticText(details, wxID_ANY, "-");
    wxFont filename_font = m_filename->GetFont();
    filename_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_filename->SetFont(filename_font);
    m_filename->SetMinSize(FromDIP(wxSize(-1, 38)));
    m_filename->Wrap(FromDIP(250));
    detail_sizer->Add(m_filename, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(4)));

    m_progress = new wxGauge(details, wxID_ANY, 1000);
    m_progress->SetMinSize(FromDIP(wxSize(-1, 12)));
    detail_sizer->Add(m_progress, wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, FromDIP(6)));
    m_layers = add_value_row(details, detail_sizer, _L("Layer: -"));
    m_time   = add_value_row(details, detail_sizer, _L("Elapsed / remaining: -"));
    m_time->SetMinSize(FromDIP(wxSize(-1, 40)));

    auto *controls = new wxBoxSizer(wxHORIZONTAL);
    m_pause_resume = new Button(details, _L("Pause"));
    m_pause_resume->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_pause_resume->SetMinSize(FromDIP(wxSize(72, 36)));
    m_pause_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_pause_resume(); });
    controls->Add(m_pause_resume, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(6)));

    m_cancel = new Button(details, _L("Cancel"), "media_stop", 0, 14);
    m_cancel->SetStyle(ButtonStyle::Alert, ButtonType::Window);
    m_cancel->SetMinSize(FromDIP(wxSize(72, 36)));
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_cancel(); });
    controls->Add(m_cancel, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(6)));

    m_refresh = new Button(details, _L("Refresh"), "refresh", 0, 14);
    m_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_refresh->SetMinSize(FromDIP(wxSize(72, 36)));
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh_now(); });
    controls->Add(m_refresh, wxSizerFlags().Expand().Proportion(1));
    detail_sizer->Add(controls, wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, FromDIP(6)));

    add_section_title(details, detail_sizer, _L("Toolhead temperature"));

    auto *tool_grid = new wxGridSizer(2, 2, FromDIP(7), FromDIP(7));
    for (size_t i = 0; i < m_toolheads.size(); ++i) {
        auto *tile = new wxPanel(details);
        tile->SetBackgroundColour(panel_alt);
        tile->SetMinSize(FromDIP(wxSize(120, 52)));
        auto *tile_sizer = new wxBoxSizer(wxVERTICAL);
        m_toolheads[i] = new wxStaticText(tile, wxID_ANY, wxString::Format("T%zu  -", i + 1));
        wxFont tool_font = m_toolheads[i]->GetFont();
        tool_font.SetWeight(wxFONTWEIGHT_BOLD);
        m_toolheads[i]->SetFont(tool_font);
        tile_sizer->Add(m_toolheads[i], wxSizerFlags().CenterVertical().Border(wxALL, FromDIP(11)));
        tile->SetSizer(tile_sizer);
        tool_grid->Add(tile, wxSizerFlags().Expand());
    }
    detail_sizer->Add(tool_grid, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(5)));

    add_section_title(details, detail_sizer, _L("Machine"));
    auto *machine_grid = new wxGridSizer(2, 2, FromDIP(7), FromDIP(7));
    auto add_machine_tile = [&](wxStaticText *&value, const wxString &label) {
        auto *tile = new wxPanel(details);
        tile->SetBackgroundColour(panel_alt);
        tile->SetMinSize(FromDIP(wxSize(120, 46)));
        auto *tile_sizer = new wxBoxSizer(wxVERTICAL);
        value = new wxStaticText(tile, wxID_ANY, label);
        tile_sizer->Add(value, wxSizerFlags().CenterVertical().Border(wxALL, FromDIP(10)));
        tile->SetSizer(tile_sizer);
        machine_grid->Add(tile, wxSizerFlags().Expand());
    };
    add_machine_tile(m_bed, _L("Bed: -"));
    add_machine_tile(m_speed, _L("Speed: -"));
    add_machine_tile(m_light, _L("Light: -"));
    add_machine_tile(m_fans, _L("Fans: -"));
    detail_sizer->Add(machine_grid, wxSizerFlags().Expand());

    detail_sizer->AddStretchSpacer();
    m_message = new wxStaticText(details, wxID_ANY, wxEmptyString);
    m_message->SetForegroundColour(muted);
    m_message->SetMinSize(FromDIP(wxSize(-1, 30)));
    m_message->Wrap(FromDIP(350));
    detail_sizer->Add(m_message, wxSizerFlags().Expand().Border(wxTOP, FromDIP(8)));
    auto *details_outer = new wxBoxSizer(wxVERTICAL);
    details_outer->Add(detail_sizer, wxSizerFlags().Expand().Proportion(1).Border(wxALL, FromDIP(16)));
    details->SetSizer(details_outer);

    main_area->Add(details, wxSizerFlags().Expand().Proportion(5));
    root->Add(main_area, wxSizerFlags().Expand().Proportion(1).Border(wxALL, FromDIP(12)));

    auto *management = new wxNotebook(scroll, wxID_ANY);
    management->SetMinSize(FromDIP(wxSize(-1, 430)));

    auto make_action = [this](wxWindow *parent, const wxString &label, const std::function<void()> &action) {
        auto *button = new Button(parent, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->SetMinSize(FromDIP(wxSize(92, 34)));
        button->Bind(wxEVT_BUTTON, [action](wxCommandEvent &) { action(); });
        m_connected_buttons.emplace_back(button);
        return button;
    };

    auto *control_page = new wxPanel(management);
    auto *control_root = new wxBoxSizer(wxHORIZONTAL);
    auto *motion_box = new wxStaticBoxSizer(wxVERTICAL, control_page, _L("Motion"));
    m_position = add_value_row(control_page, motion_box, _L("Position: -"));
    m_homed_axes = add_value_row(control_page, motion_box, _L("Homed axes: none"));
    auto *home_row = new wxBoxSizer(wxHORIZONTAL);
    m_home_all = make_action(control_page, _L("Home all"), [this]() { on_home(""); });
    home_row->Add(m_home_all, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(5)));
    for (const char axis : {'X', 'Y', 'Z'}) {
        auto *button = make_action(
            control_page,
            wxString::Format("Home %c", axis),
            [this, axis]() { on_home(std::string(1, axis)); });
        home_row->Add(button, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(5)));
    }
    motion_box->Add(home_row, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(8)));

    auto *step_row = new wxBoxSizer(wxHORIZONTAL);
    step_row->Add(new wxStaticText(control_page, wxID_ANY, _L("Jog distance")),
                  wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(8)));
    m_jog_distance = new wxChoice(control_page, wxID_ANY);
    for (const wxString &step : std::array<wxString, 4> {"0.1 mm", "1 mm", "10 mm", "50 mm"})
        m_jog_distance->Append(step);
    m_jog_distance->SetSelection(2);
    step_row->Add(m_jog_distance, wxSizerFlags().Expand().Proportion(1));
    motion_box->Add(step_row, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(8)));

    auto *jog_grid = new wxGridSizer(3, 2, FromDIP(6), FromDIP(6));
    for (const char axis : {'X', 'Y', 'Z'}) {
        for (const double direction : {-1.0, 1.0}) {
            const wxString label = wxString::Format("%c%c", axis, direction < 0.0 ? '-' : '+');
            auto *button = make_action(control_page, label, [this, axis, direction]() { on_jog(axis, direction); });
            m_jog_buttons.emplace_back(button);
            jog_grid->Add(button, wxSizerFlags().Expand());
        }
    }
    motion_box->Add(jog_grid, wxSizerFlags().Expand());
    auto *motors_off = make_action(control_page, _L("Disable motors"), [this]() {
        send_gcode_script("M84", _L("Motors disabled"));
    });
    motion_box->Add(motors_off, wxSizerFlags().Expand().Border(wxTOP, FromDIP(8)));
    control_root->Add(motion_box, wxSizerFlags().Expand().Proportion(4).Border(wxALL, FromDIP(10)));

    auto *temperature_box = new wxStaticBoxSizer(wxVERTICAL, control_page, _L("Temperature"));
    auto *temperature_grid = new wxFlexGridSizer(5, 3, FromDIP(7), FromDIP(8));
    temperature_grid->AddGrowableCol(1, 1);
    for (size_t i = 0; i < m_temperature_targets.size(); ++i) {
        const bool bed = i + 1 == m_temperature_targets.size();
        temperature_grid->Add(
            new wxStaticText(control_page, wxID_ANY, bed ? _L("Bed") : wxString::Format("T%zu", i + 1)),
            wxSizerFlags().CenterVertical());
        m_temperature_targets[i] = new wxSpinCtrlDouble(
            control_page,
            wxID_ANY,
            "0",
            wxDefaultPosition,
            FromDIP(wxSize(100, -1)),
            wxSP_ARROW_KEYS,
            0.0,
            bed ? 150.0 : 350.0,
            0.0,
            5.0);
        m_temperature_targets[i]->SetDigits(0);
        temperature_grid->Add(m_temperature_targets[i], wxSizerFlags().Expand());
        auto *set_button = make_action(control_page, _L("Set"), [this, i]() { on_set_temperature(i); });
        temperature_grid->Add(set_button, wxSizerFlags().Expand());
    }
    temperature_box->Add(temperature_grid, wxSizerFlags().Expand());
    auto *heaters_off = make_action(control_page, _L("Turn off heaters"), [this]() {
        send_gcode_script("TURN_OFF_HEATERS", _L("Heaters turned off"));
    });
    temperature_box->Add(heaters_off, wxSizerFlags().Expand().Border(wxTOP, FromDIP(8)));
    control_root->Add(temperature_box, wxSizerFlags().Expand().Proportion(4).Border(wxTOP | wxBOTTOM, FromDIP(10)));

    auto *tuning_box = new wxStaticBoxSizer(wxVERTICAL, control_page, _L("Tuning"));
    auto add_tuning = [&](const wxString &label, wxSpinCtrlDouble *&control, double min, double max, double value) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(control_page, wxID_ANY, label),
                 wxSizerFlags().CenterVertical().Proportion(1).Border(wxRIGHT, FromDIP(8)));
        control = new wxSpinCtrlDouble(
            control_page, wxID_ANY, wxString::Format("%.0f", value), wxDefaultPosition,
            FromDIP(wxSize(92, -1)), wxSP_ARROW_KEYS, min, max, value, 5.0);
        control->SetDigits(0);
        row->Add(control, wxSizerFlags().CenterVertical());
        tuning_box->Add(row, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(6)));
    };
    add_tuning(_L("Speed") + " (%)", m_speed_target, 10.0, 300.0, 100.0);
    add_tuning(_L("Flow") + " (%)", m_flow_target, 50.0, 150.0, 100.0);
    auto *apply_tuning = make_action(control_page, _L("Apply speed / flow"), [this]() { on_set_tuning(); });
    tuning_box->Add(apply_tuning, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(10)));
    add_tuning(_L("Part fan") + " (%)", m_main_fan_target, 0.0, 100.0, 0.0);
    add_tuning(_L("Cavity fan") + " (%)", m_cavity_fan_target, 0.0, 100.0, 0.0);
    add_tuning(_L("Chamber light") + " (%)", m_light_target, 0.0, 100.0, 0.0);
    auto *apply_fans = make_action(control_page, _L("Apply fans / light"), [this]() { on_set_fans(); });
    tuning_box->Add(apply_fans, wxSizerFlags().Expand());
    control_root->Add(tuning_box, wxSizerFlags().Expand().Proportion(4).Border(wxALL, FromDIP(10)));
    control_page->SetSizer(control_root);
    management->AddPage(control_page, _L("Control"), true);

    auto *console_page = new wxPanel(management);
    auto *console_root = new wxBoxSizer(wxHORIZONTAL);
    auto *macro_box = new wxStaticBoxSizer(wxVERTICAL, console_page, _L("Macros"));
    m_macro_list = new wxListBox(console_page, wxID_ANY);
    m_macro_list->SetMinSize(FromDIP(wxSize(300, 245)));
    m_macro_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { update_controls(); });
    macro_box->Add(m_macro_list, wxSizerFlags().Expand().Proportion(1));
    m_macro_arguments = new wxTextCtrl(console_page, wxID_ANY);
    m_macro_arguments->SetHint(_L("Optional arguments, for example: X=10"));
    macro_box->Add(m_macro_arguments, wxSizerFlags().Expand().Border(wxTOP, FromDIP(7)));
    m_macro_run = make_action(console_page, _L("Run selected macro"), [this]() { on_run_macro(); });
    macro_box->Add(m_macro_run, wxSizerFlags().Expand().Border(wxTOP, FromDIP(7)));
    console_root->Add(macro_box, wxSizerFlags().Expand().Proportion(4).Border(wxALL, FromDIP(10)));

    auto *command_box = new wxStaticBoxSizer(wxVERTICAL, console_page, _L("Console"));
    m_console_output = new wxTextCtrl(
        console_page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    command_box->Add(m_console_output, wxSizerFlags().Expand().Proportion(1));
    auto *command_row = new wxBoxSizer(wxHORIZONTAL);
    m_console_command = new wxTextCtrl(console_page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_console_command->SetHint(_L("G-code command"));
    m_console_command->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { on_send_console(); });
    command_row->Add(m_console_command, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(7)));
    m_console_send = make_action(console_page, _L("Send"), [this]() { on_send_console(); });
    command_row->Add(m_console_send, wxSizerFlags().Expand());
    command_box->Add(command_row, wxSizerFlags().Expand().Border(wxTOP, FromDIP(7)));
    console_root->Add(command_box, wxSizerFlags().Expand().Proportion(8).Border(wxALL, FromDIP(10)));
    console_page->SetSizer(console_root);
    management->AddPage(console_page, _L("Console / Macros"), false);

    auto *files_page = new wxPanel(management);
    auto *files_root = new wxBoxSizer(wxHORIZONTAL);
    auto *files_box = new wxStaticBoxSizer(wxVERTICAL, files_page, _L("G-code files"));
    m_file_list = new wxListBox(files_page, wxID_ANY);
    m_file_list->SetMinSize(FromDIP(wxSize(450, 250)));
    m_file_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        const int selection = m_file_list->GetSelection();
        m_file_details->SetLabel(
            selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_file_names.size()
                ? _L("Select a file")
                : from_u8(m_file_names[static_cast<size_t>(selection)]));
        update_controls();
    });
    files_box->Add(m_file_list, wxSizerFlags().Expand().Proportion(1));
    m_file_details = add_value_row(files_page, files_box, _L("Select a file"));
    auto *file_buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *refresh_files = make_action(files_page, _L("Refresh"), [this]() {
        request_files();
        request_history();
    });
    file_buttons->Add(refresh_files, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(6)));
    m_file_print = make_action(files_page, _L("Print"), [this]() { on_start_file(); });
    m_file_print->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    file_buttons->Add(m_file_print, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(6)));
    m_file_delete = make_action(files_page, _L("Delete"), [this]() { on_delete_file(); });
    m_file_delete->SetStyle(ButtonStyle::Alert, ButtonType::Window);
    file_buttons->Add(m_file_delete, wxSizerFlags().Expand().Proportion(1));
    files_box->Add(file_buttons, wxSizerFlags().Expand());
    files_root->Add(files_box, wxSizerFlags().Expand().Proportion(7).Border(wxALL, FromDIP(10)));

    auto *history_box = new wxStaticBoxSizer(wxVERTICAL, files_page, _L("Job history"));
    m_history_list = new wxListBox(files_page, wxID_ANY);
    history_box->Add(m_history_list, wxSizerFlags().Expand().Proportion(1));
    files_root->Add(history_box, wxSizerFlags().Expand().Proportion(5).Border(wxALL, FromDIP(10)));
    files_page->SetSizer(files_root);
    management->AddPage(files_page, _L("Files / History"), false);

    auto *machine_page = new wxPanel(management);
    auto *machine_root = new wxBoxSizer(wxHORIZONTAL);
    auto *mesh_box = new wxStaticBoxSizer(wxVERTICAL, machine_page, _L("Bed mesh"));
    m_mesh_status = add_value_row(machine_page, mesh_box, _L("Mesh: -"));
    auto *mesh_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_mesh_calibrate = make_action(machine_page, _L("Calibrate"), [this]() {
        if (wxMessageBox(_L("Run bed mesh calibration now?"), _L("Bed mesh"),
                         wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES)
            send_gcode_script("BED_MESH_CALIBRATE", _L("Bed mesh calibration started"));
    });
    mesh_buttons->Add(m_mesh_calibrate, wxSizerFlags().Expand().Proportion(1).Border(wxRIGHT, FromDIP(6)));
    m_mesh_clear = make_action(machine_page, _L("Clear"), [this]() {
        send_gcode_script("BED_MESH_CLEAR", _L("Bed mesh cleared"));
    });
    mesh_buttons->Add(m_mesh_clear, wxSizerFlags().Expand().Proportion(1));
    mesh_box->Add(mesh_buttons, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(8)));
    m_endstop_status = add_value_row(machine_page, mesh_box, _L("Endstops: -"));
    auto *query_endstops = make_action(machine_page, _L("Query endstops"), [this]() {
        send_gcode_script("QUERY_ENDSTOPS", _L("Endstop query sent"));
    });
    mesh_box->Add(query_endstops, wxSizerFlags().Expand());
    machine_root->Add(mesh_box, wxSizerFlags().Expand().Proportion(5).Border(wxALL, FromDIP(10)));

    auto *system_box = new wxStaticBoxSizer(wxVERTICAL, machine_page, _L("Machine"));
    m_system_summary = new wxStaticText(machine_page, wxID_ANY, _L("System information unavailable"));
    m_system_summary->Wrap(FromDIP(500));
    system_box->Add(m_system_summary, wxSizerFlags().Expand().Proportion(1).Border(wxBOTTOM, FromDIP(8)));
    auto *refresh_system = make_action(machine_page, _L("Refresh machine status"), [this]() {
        request_system_info();
        request_status();
    });
    system_box->Add(refresh_system, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(7)));
    auto *firmware_restart = make_action(machine_page, _L("Firmware restart"), [this]() {
        if (wxMessageBox(_L("Restart printer firmware?"), _L("Firmware restart"),
                         wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES)
            send_gcode_script("FIRMWARE_RESTART", _L("Firmware restart requested"));
    });
    system_box->Add(firmware_restart, wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(7)));
    m_emergency_stop = make_action(machine_page, _L("Emergency stop"), [this]() { on_emergency_stop(); });
    m_emergency_stop->SetStyle(ButtonStyle::Alert, ButtonType::Window);
    system_box->Add(m_emergency_stop, wxSizerFlags().Expand());
    machine_root->Add(system_box, wxSizerFlags().Expand().Proportion(7).Border(wxALL, FromDIP(10)));
    machine_page->SetSizer(machine_root);
    management->AddPage(machine_page, _L("Machine"), false);

    root->Add(management, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12)));
    Bind(wxEVT_SIZE, [this, main_area, scroll](wxSizeEvent &event) {
        const int orientation = event.GetSize().x >= FromDIP(1400) ? wxHORIZONTAL : wxVERTICAL;
        if (main_area->GetOrientation() != orientation) {
            main_area->SetOrientation(orientation);
            Layout();
            scroll->FitInside();
        }
        event.Skip();
    });

    auto *footer = new wxPanel(scroll);
    footer->SetBackgroundColour(panel_bg);
    footer->SetMinSize(FromDIP(wxSize(-1, 38)));
    auto *footer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_camera_status = new wxStaticText(footer, wxID_ANY, _L("Camera: waiting"));
    m_active_tool = new wxStaticText(footer, wxID_ANY, _L("Active toolhead: -"));
    m_network_status = new wxStaticText(footer, wxID_ANY, _L("LAN: -"));
    m_camera_status->SetMinSize(FromDIP(wxSize(220, -1)));
    m_active_tool->SetMinSize(FromDIP(wxSize(160, -1)));
    m_network_status->SetMinSize(FromDIP(wxSize(110, -1)));
    m_camera_status->SetForegroundColour(muted);
    m_active_tool->SetForegroundColour(muted);
    m_network_status->SetForegroundColour(muted);
    footer_sizer->Add(m_camera_status, wxSizerFlags().CenterVertical().Border(wxLEFT, FromDIP(14)));
    footer_sizer->AddStretchSpacer();
    footer_sizer->Add(m_active_tool, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(28)));
    footer_sizer->Add(m_network_status, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(14)));
    footer->SetSizer(footer_sizer);
    root->Add(footer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12)));
    scroll->SetSizer(root);
    scroll->FitInside();
    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(scroll, wxSizerFlags().Expand().Proportion(1));
    SetSizer(outer);
    update_controls();
}

void SnapmakerMonitorPanel::set_server(const wxString &url, const wxString &api_key)
{
    const std::string base_url = normalize_base_url(url);
    const std::string key = api_key.ToUTF8().data();
    if (base_url == m_base_url && key == m_api_key)
        return;

    stop_camera_session();
    m_base_url = base_url;
    m_api_key = key;
    ++m_server_generation;
    m_status_objects.clear();
    m_status_in_flight = false;
    m_camera_in_flight = false;
    m_objects_in_flight = false;
    m_command_in_flight = false;
    m_gcode_in_flight = false;
    m_console_in_flight = false;
    m_files_in_flight = false;
    m_history_in_flight = false;
    m_system_in_flight = false;
    m_exclude_object_supported = false;
    m_connected = false;
    m_homed = false;
    m_bed_mesh_supported = false;
    m_cavity_fan_supported = false;
    m_light_supported = false;
    m_slow_refresh_tick = 0;
    m_print_state.clear();
    m_current_layer = 0;
    m_total_layers = 0;
    m_displayed_layer = 0;
    m_gcode_requested_filename.clear();
    m_gcode_cache_filename.clear();
    m_gcode_cache.reset();
    m_object_names.clear();
    m_object_list_signature.clear();
    m_macro_names.clear();
    m_file_names.clear();
    m_object_list->Clear();
    m_macro_list->Clear();
    m_file_list->Clear();
    m_history_list->Clear();
    m_console_output->Clear();
    m_temperature_history->clear();
    m_layer_view->set_status(_L("Waiting for G-code"));
    m_layer_view->set_objects({});
    m_camera_interval_ms = CAMERA_INTERVAL_MS;
    m_camera_frames_in_window = 0;
    m_camera_fps = 0.0;
    m_camera_latency_ms = 0;
    m_last_frame_hash = 0;
    m_camera_request_sequence = 0;
    m_fps_window_started = std::chrono::steady_clock::now();
    m_connection->SetLabel(_L("Connecting..."));
    m_state->SetLabel(_L("Unavailable"));
    update_controls();
    update_footer();

    if (m_active && !m_base_url.empty()) {
        start_camera_session();
        request_object_list();
        request_camera();
        refresh_auxiliary();
    }
}

void SnapmakerMonitorPanel::set_active(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active && !m_base_url.empty()) {
        start_camera_session();
        request_object_list();
        request_camera();
        refresh_auxiliary();
        m_status_timer.Start(POLL_INTERVAL_MS);
        m_camera_timer.Start(m_camera_interval_ms);
    } else {
        m_status_timer.Stop();
        m_camera_timer.Stop();
        stop_camera_session();
    }
}

void SnapmakerMonitorPanel::refresh_now()
{
    if (!m_active || m_base_url.empty())
        return;
    start_camera_session();
    if (m_status_objects.empty())
        request_object_list();
    else
        request_status();
    request_camera();
    refresh_auxiliary();
}

void SnapmakerMonitorPanel::start_camera_session()
{
    if (!m_active || m_base_url.empty() || m_camera_session->is_running())
        return;

    m_camera_session_status = _L("Camera: connecting");
    update_footer();
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    const std::uint64_t session_generation = ++m_camera_session_generation;
    m_camera_session->start(
        m_base_url,
        m_api_key,
        [this, lifetime, generation, session_generation](SnapmakerCameraSession::State state, const std::string &message) {
            wxTheApp->CallAfter([this, lifetime, generation, session_generation, state, message]() {
                if (lifetime.expired() || generation != m_server_generation ||
                    session_generation != m_camera_session_generation)
                    return;
                switch (state) {
                case SnapmakerCameraSession::State::Starting:
                    m_camera_session_status = _L("Camera: connecting");
                    break;
                case SnapmakerCameraSession::State::Active:
                    m_camera_session_status = _L("Camera: waiting for frames");
                    m_camera_interval_ms = CAMERA_INTERVAL_MS;
                    if (m_camera_timer.IsRunning())
                        m_camera_timer.Start(m_camera_interval_ms);
                    request_camera();
                    break;
                case SnapmakerCameraSession::State::Reconnecting:
                    m_camera_session_status = _L("Camera: reconnecting");
                    break;
                case SnapmakerCameraSession::State::Stopped:
                    m_camera_session_status = _L("Camera: stopped");
                    break;
                case SnapmakerCameraSession::State::Error:
                    m_camera_session_status = message.empty()
                        ? _L("Camera: unavailable")
                        : _L("Camera: unavailable") + " (" + from_u8(message) + ")";
                    break;
                }
                update_footer();
            });
        });
}

void SnapmakerMonitorPanel::stop_camera_session()
{
    ++m_camera_session_generation;
    if (m_camera_session)
        m_camera_session->stop();
}

void SnapmakerMonitorPanel::request_object_list()
{
    if (m_objects_in_flight || m_base_url.empty())
        return;
    m_objects_in_flight = true;
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::get(m_base_url + "/printer/objects/list");
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(2)
        .timeout_max(4)
        .on_complete([this, lifetime, generation](std::string body, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, body = std::move(body)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_objects_in_flight = false;
                apply_object_list(body);
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_objects_in_flight = false;
                set_connection_error(from_u8(error.empty() ? "Printer is unavailable" : error));
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::request_status()
{
    if (m_status_in_flight || m_status_objects.empty())
        return;
    m_status_in_flight = true;
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::get(make_status_url());
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(2)
        .timeout_max(4)
        .on_complete([this, lifetime, generation](std::string body, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, body = std::move(body)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_status_in_flight = false;
                apply_status(body);
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_status_in_flight = false;
                set_connection_error(from_u8(error.empty() ? "Status request failed" : error));
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::request_auxiliary(
    const std::string &endpoint,
    bool &in_flight,
    std::function<void(const std::string &)> apply)
{
    if (in_flight || m_base_url.empty())
        return;
    in_flight = true;
    bool *const request_flag = &in_flight;
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::get(m_base_url + endpoint);
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(2)
        .timeout_max(6)
        .size_limit(16 * 1024 * 1024)
        .on_complete([this, lifetime, generation, request_flag, apply = std::move(apply)](std::string body, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, request_flag, apply, body = std::move(body)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                *request_flag = false;
                apply(body);
            });
        })
        .on_error([this, lifetime, generation, request_flag](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, request_flag, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                *request_flag = false;
                if (!error.empty())
                    m_message->SetLabel(_L("Auxiliary data unavailable") + ": " + from_u8(error));
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::request_console()
{
    request_auxiliary(
        "/server/gcode_store?count=100",
        m_console_in_flight,
        [this](const std::string &body) { apply_console(body); });
}

void SnapmakerMonitorPanel::request_files()
{
    request_auxiliary(
        "/server/files/list?root=gcodes",
        m_files_in_flight,
        [this](const std::string &body) { apply_files(body); });
}

void SnapmakerMonitorPanel::request_history()
{
    request_auxiliary(
        "/server/history/list?limit=50",
        m_history_in_flight,
        [this](const std::string &body) { apply_history(body); });
}

void SnapmakerMonitorPanel::request_system_info()
{
    request_auxiliary(
        "/machine/system_info",
        m_system_in_flight,
        [this](const std::string &body) { apply_system_info(body); });
}

void SnapmakerMonitorPanel::request_camera()
{
    if (m_camera_in_flight || m_base_url.empty())
        return;
    m_camera_in_flight = true;
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    const auto started = std::chrono::steady_clock::now();
    const wxSize target = m_camera->GetClientSize();
    const int target_width = std::max(1, target.x);
    const int target_height = std::max(1, target.y);
    const std::uint64_t previous_hash = m_last_frame_hash;
    const std::string url = m_base_url + "/server/files/camera/monitor.jpg?t=" +
                            std::to_string(++m_camera_request_sequence);
    auto request = Slic3r::Http::get(url);
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(2)
        .timeout_max(5)
        .size_limit(8 * 1024 * 1024)
        .on_complete([this, lifetime, generation, started, target_width, target_height, previous_hash](std::string body, unsigned) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            const std::uint64_t hash = frame_hash(body);
            if (hash == previous_hash) {
                wxTheApp->CallAfter([this, lifetime, generation, elapsed]() {
                    if (lifetime.expired() || generation != m_server_generation)
                        return;
                    m_camera_in_flight = false;
                    apply_duplicate_camera_frame(static_cast<long>(elapsed));
                });
                return;
            }

            wxMemoryInputStream stream(body.data(), body.size());
            wxImage image;
            if (image.LoadFile(stream, wxBITMAP_TYPE_JPEG) && image.IsOk()) {
                const double scale = std::min(
                    static_cast<double>(target_width) / image.GetWidth(),
                    static_cast<double>(target_height) / image.GetHeight());
                const int width = std::max(1, static_cast<int>(std::lround(image.GetWidth() * scale)));
                const int height = std::max(1, static_cast<int>(std::lround(image.GetHeight() * scale)));
                if (width != image.GetWidth() || height != image.GetHeight())
                    image.Rescale(width, height, wxIMAGE_QUALITY_HIGH);
            }
            wxTheApp->CallAfter([this, lifetime, generation, image = std::move(image), hash, elapsed]() mutable {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_camera_in_flight = false;
                if (image.IsOk())
                    apply_camera(std::move(image), hash, static_cast<long>(elapsed));
                else
                    apply_duplicate_camera_frame(static_cast<long>(elapsed));
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation]() {
                if (!lifetime.expired() && generation == m_server_generation)
                    m_camera_in_flight = false;
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::request_gcode(const std::string &filename)
{
    if (filename.empty() || m_base_url.empty())
        return;
    m_gcode_requested_filename = filename;
    if (m_gcode_in_flight || (filename == m_gcode_cache_filename && m_gcode_cache))
        return;

    m_gcode_in_flight = true;
    m_displayed_layer = 0;
    m_layer_view->set_status(_L("Loading current G-code..."));

    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    const std::string url = m_base_url + "/server/files/gcodes/" + encode_url_path(filename);
    auto request = Slic3r::Http::get(url);
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(3)
        .timeout_max(120)
        .size_limit(MAX_GCODE_BYTES)
        .on_complete([this, lifetime, generation, filename](std::string body, unsigned) {
            std::shared_ptr<SnapmakerGCodeLayerCache> cache;
            std::string parse_error;
            try {
                cache = SnapmakerGCodeLayerCache::parse(std::move(body));
                if (!cache || cache->empty())
                    parse_error = "No G-code layers found";
            } catch (const std::exception &e) {
                parse_error = e.what();
            }
            wxTheApp->CallAfter([
                this,
                lifetime,
                generation,
                filename,
                cache = std::move(cache),
                parse_error = std::move(parse_error)
            ]() mutable {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_gcode_in_flight = false;
                if (filename != m_gcode_requested_filename) {
                    request_gcode(m_gcode_requested_filename);
                    return;
                }
                if (!parse_error.empty()) {
                    m_gcode_cache.reset();
                    m_gcode_cache_filename.clear();
                    m_layer_view->set_status(_L("G-code layer unavailable") + ": " + from_u8(parse_error));
                    return;
                }
                m_gcode_cache = std::move(cache);
                m_gcode_cache_filename = filename;
                m_displayed_layer = 0;
                update_layer_view(m_current_layer, m_total_layers);
            });
        })
        .on_error([this, lifetime, generation, filename](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, filename, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_gcode_in_flight = false;
                if (filename != m_gcode_requested_filename) {
                    request_gcode(m_gcode_requested_filename);
                    return;
                }
                m_gcode_cache.reset();
                m_gcode_cache_filename.clear();
                wxString message = _L("G-code layer unavailable");
                if (!error.empty())
                    message += ": " + from_u8(error);
                m_layer_view->set_status(message);
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::apply_object_list(const std::string &body)
{
    try {
        const json data = json::parse(body);
        const auto &objects = data.at("result").at("objects");
        if (!objects.is_array())
            throw std::runtime_error("Invalid object list");

        static const std::array<std::string, 12> required {
            "print_stats", "display_status", "virtual_sdcard", "toolhead", "heater_bed", "gcode_move",
            "exclude_object", "bed_mesh", "system_stats", "query_endstops", "temperature_sensor cavity",
            "idle_timeout"
        };
        m_status_objects.clear();
        m_macro_names.clear();
        m_bed_mesh_supported = false;
        m_cavity_fan_supported = false;
        m_light_supported = false;
        for (const auto &name : required) {
            if (std::find(objects.begin(), objects.end(), name) != objects.end())
                m_status_objects.emplace_back(name);
        }
        for (const auto &entry : objects) {
            if (!entry.is_string())
                continue;
            const std::string name = entry.get<std::string>();
            if (is_extruder_object(name) ||
                name == "fan" || starts_with(name, "fan_generic ") ||
                starts_with(name, "output_pin ") || starts_with(name, "led "))
                m_status_objects.emplace_back(name);
            if (starts_with(name, "gcode_macro ")) {
                const std::string macro = name.substr(std::string("gcode_macro ").size());
                if (is_public_snapmaker_macro(macro))
                    m_macro_names.emplace_back(macro);
            }
            m_bed_mesh_supported = m_bed_mesh_supported || name == "bed_mesh";
            m_cavity_fan_supported = m_cavity_fan_supported || name == "fan_generic cavity_fan";
            m_light_supported = m_light_supported ||
                name == "led cavity_led" || name == "output_pin cavity_led";
        }
        std::sort(m_status_objects.begin(), m_status_objects.end());
        m_status_objects.erase(std::unique(m_status_objects.begin(), m_status_objects.end()), m_status_objects.end());
        std::sort(m_macro_names.begin(), m_macro_names.end());
        m_macro_list->Freeze();
        m_macro_list->Clear();
        for (const std::string &macro : m_macro_names)
            m_macro_list->Append(from_u8(macro));
        m_macro_list->Thaw();
        if (m_status_objects.empty())
            throw std::runtime_error("No status objects");
        request_status();
        refresh_auxiliary();
    } catch (const std::exception &e) {
        set_connection_error(from_u8(e.what()));
    }
}

void SnapmakerMonitorPanel::apply_status(const std::string &body)
{
    try {
        const json data = json::parse(body);
        const json &status = data.at("result").at("status");
        const json empty = json::object();
        const json &print_stats = status.contains("print_stats") ? status.at("print_stats") : empty;
        const json &display = status.contains("display_status") ? status.at("display_status") : empty;
        const json &virtual_sd = status.contains("virtual_sdcard") ? status.at("virtual_sdcard") : empty;

        const std::string state = string_or(print_stats, "state");
        const std::string filename = string_or(print_stats, "filename");
        const std::string active_extruder = status.contains("toolhead")
            ? string_or(status.at("toolhead"), "extruder")
            : std::string();
        double progress = number_or(virtual_sd, "progress", number_or(display, "progress"));
        progress = std::clamp(progress, 0.0, 1.0);

        m_connected = true;
        m_print_state = state;
        m_connection->SetLabel(_L("Connected"));
        m_connection->SetForegroundColour(ACCENT_BLUE);
        m_state->SetLabel(display_state(state));
        m_state->SetForegroundColour(
            state == "error" ? wxColour(218, 82, 82) :
            state == "paused" ? wxColour(224, 166, 67) :
            ACCENT_BLUE);
        m_filename->SetLabel(filename.empty() ? "-" : from_u8(filename));
        m_filename->Wrap(FromDIP(250));
        m_progress->SetValue(static_cast<int>(std::lround(progress * 1000.0)));
        m_progress_percent->SetLabel(wxString::Format("%.1f%%", progress * 100.0));

        int current_layer = 0;
        int total_layer = 0;
        const auto info_it = print_stats.find("info");
        if (info_it != print_stats.end() && info_it->is_object()) {
            current_layer = static_cast<int>(number_or(*info_it, "current_layer"));
            total_layer = static_cast<int>(number_or(*info_it, "total_layer"));
        }
        m_layers->SetLabel(wxString::Format("%s: %d / %d", _L("Layer"), current_layer, total_layer));
        m_current_layer = current_layer;
        m_total_layers = total_layer;

        const auto toolhead_it = status.find("toolhead");
        if (toolhead_it != status.end() && toolhead_it->is_object()) {
            const std::string homed_axes = string_or(*toolhead_it, "homed_axes");
            m_homed = homed_axes.find('x') != std::string::npos &&
                      homed_axes.find('y') != std::string::npos &&
                      homed_axes.find('z') != std::string::npos;
            m_homed_axes->SetLabel(_L("Homed axes") + ": " +
                                   (homed_axes.empty() ? _L("none") : from_u8(uppercase_copy(homed_axes))));
            const auto position_it = toolhead_it->find("position");
            if (position_it != toolhead_it->end() && position_it->is_array() && position_it->size() >= 3 &&
                (*position_it)[0].is_number() && (*position_it)[1].is_number() && (*position_it)[2].is_number()) {
                m_position->SetLabel(wxString::Format(
                    "%s: X %.2f  Y %.2f  Z %.2f",
                    _L("Position"),
                    (*position_it)[0].get<double>(),
                    (*position_it)[1].get<double>(),
                    (*position_it)[2].get<double>()));
            }
            const auto axis_min_it = toolhead_it->find("axis_minimum");
            const auto axis_max_it = toolhead_it->find("axis_maximum");
            if (axis_min_it != toolhead_it->end() && axis_max_it != toolhead_it->end() &&
                axis_min_it->is_array() && axis_max_it->is_array() &&
                axis_min_it->size() >= 2 && axis_max_it->size() >= 2 &&
                (*axis_min_it)[0].is_number() && (*axis_min_it)[1].is_number() &&
                (*axis_max_it)[0].is_number() && (*axis_max_it)[1].is_number()) {
                const double min_x = (*axis_min_it)[0].get<double>();
                const double min_y = (*axis_min_it)[1].get<double>();
                const double max_x = (*axis_max_it)[0].get<double>();
                double max_y = (*axis_max_it)[1].get<double>();
                const double bed_width = max_x - min_x;
                const double y_travel = max_y - min_y;
                // U1's Y travel includes the rear wiping/service zone, outside the printable bed.
                if (bed_width > 0.0 && y_travel > bed_width * 1.15)
                    max_y = min_y + bed_width;
                m_layer_view->set_bed_bounds(min_x, min_y, max_x, max_y);
            }
        }

        std::vector<ExcludeObjectShape> exclude_objects;
        m_exclude_object_supported = status.contains("exclude_object") && status.at("exclude_object").is_object();
        if (m_exclude_object_supported) {
            const json &exclude_status = status.at("exclude_object");
            std::unordered_set<std::string> excluded_names;
            const auto excluded_it = exclude_status.find("excluded_objects");
            if (excluded_it != exclude_status.end() && excluded_it->is_array()) {
                for (const auto &name : *excluded_it)
                    if (name.is_string())
                        excluded_names.insert(uppercase_copy(name.get<std::string>()));
            }

            const auto objects_it = exclude_status.find("objects");
            if (objects_it != exclude_status.end() && objects_it->is_array()) {
                for (const auto &entry : *objects_it) {
                    if (!entry.is_object())
                        continue;
                    ExcludeObjectShape object;
                    object.name = string_or(entry, "name");
                    const auto polygon_it = entry.find("polygon");
                    if (object.name.empty() || polygon_it == entry.end() || !polygon_it->is_array())
                        continue;
                    for (const auto &point : *polygon_it) {
                        if (!point.is_array() || point.size() < 2 || !point[0].is_number() || !point[1].is_number())
                            continue;
                        object.polygon.push_back({point[0].get<double>(), point[1].get<double>()});
                    }
                    if (object.polygon.size() < 3)
                        continue;
                    object.excluded = excluded_names.find(uppercase_copy(object.name)) != excluded_names.end();
                    exclude_objects.emplace_back(std::move(object));
                }
            }
        }
        update_object_list(exclude_objects);
        m_layer_view->set_objects(std::move(exclude_objects));

        if (!filename.empty() && (state == "printing" || state == "paused")) {
            if (filename != m_gcode_cache_filename)
                request_gcode(filename);
            else
                update_layer_view(current_layer, total_layer);
        } else {
            m_layer_view->set_status(_L("No active G-code"));
            m_displayed_layer = 0;
        }

        const double elapsed = number_or(print_stats, "print_duration", number_or(print_stats, "total_duration"));
        const double remaining = progress > 0.001 ? std::max(0.0, elapsed / progress - elapsed) : 0.0;
        m_time->SetLabel(_L("Elapsed") + ": " + duration_text(elapsed) + "\n" +
                         _L("Remaining") + ": " + duration_text(remaining));

        for (size_t i = 0; i < m_toolheads.size(); ++i) {
            const std::string object_name = i == 0 ? "extruder" : "extruder" + std::to_string(i);
            const wxString label = wxString::Format("T%zu%s", i + 1, active_extruder == object_name ? " *" : "");
            if (status.contains(object_name))
                m_toolheads[i]->SetLabel(temperature_text(label, status.at(object_name)));
            else
                m_toolheads[i]->SetLabel(label + "  -");
            m_toolheads[i]->SetForegroundColour(active_extruder == object_name ? ACCENT_BLUE : wxNullColour);
            if (status.contains(object_name) && m_temperature_targets[i] != nullptr &&
                wxWindow::FindFocus() != m_temperature_targets[i])
                m_temperature_targets[i]->SetValue(number_or(status.at(object_name), "target"));
        }
        wxString active_tool = "-";
        for (size_t i = 0; i < m_toolheads.size(); ++i) {
            const std::string object_name = i == 0 ? "extruder" : "extruder" + std::to_string(i);
            if (active_extruder == object_name) {
                active_tool = wxString::Format("T%zu", i + 1);
                break;
            }
        }
        m_active_tool->SetLabel(_L("Active toolhead") + ": " + active_tool);
        m_bed->SetLabel(status.contains("heater_bed")
            ? temperature_text(_L("Bed"), status.at("heater_bed"))
            : _L("Bed: -"));
        if (status.contains("heater_bed") && m_temperature_targets.back() != nullptr &&
            wxWindow::FindFocus() != m_temperature_targets.back())
            m_temperature_targets.back()->SetValue(number_or(status.at("heater_bed"), "target"));

        std::array<double, 5> temperatures;
        temperatures.fill(std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 0; i < m_toolheads.size(); ++i) {
            const std::string object_name = i == 0 ? "extruder" : "extruder" + std::to_string(i);
            if (status.contains(object_name))
                temperatures[i] = number_or(
                    status.at(object_name),
                    "temperature",
                    std::numeric_limits<double>::quiet_NaN());
        }
        if (status.contains("heater_bed"))
            temperatures.back() = number_or(
                status.at("heater_bed"),
                "temperature",
                std::numeric_limits<double>::quiet_NaN());
        m_temperature_history->add_sample(temperatures);

        std::vector<wxString> fans;
        wxString light = _L("Light: -");
        for (auto it = status.begin(); it != status.end(); ++it) {
            const std::string &name = it.key();
            if (name == "fan" || starts_with(name, "fan_generic ")) {
                const int speed = static_cast<int>(std::lround(number_or(it.value(), "speed") * 100.0));
                const std::string display_name = name == "fan" ? "Main" : name.substr(std::string("fan_generic ").size());
                fans.emplace_back(wxString::Format("%s %d%%", from_u8(display_name), speed));
                if (name == "fan" && m_main_fan_target != nullptr && wxWindow::FindFocus() != m_main_fan_target)
                    m_main_fan_target->SetValue(speed);
                if (name == "fan_generic cavity_fan" && m_cavity_fan_target != nullptr &&
                    wxWindow::FindFocus() != m_cavity_fan_target)
                    m_cavity_fan_target->SetValue(speed);
            } else if (starts_with(name, "output_pin ")) {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("light") != std::string::npos || lower.find("led") != std::string::npos) {
                    const int value = static_cast<int>(std::lround(number_or(it.value(), "value") * 100.0));
                    light = wxString::Format("%s: %d%%", _L("Light"), value);
                    if (m_light_target != nullptr && wxWindow::FindFocus() != m_light_target)
                        m_light_target->SetValue(value);
                }
            } else if (starts_with(name, "led ")) {
                const auto colors = it.value().find("color_data");
                if (colors != it.value().end() && colors->is_array() && !colors->empty() &&
                    (*colors)[0].is_array() && !(*colors)[0].empty()) {
                    const json &channels = (*colors)[0];
                    const size_t channel = channels.size() >= 4 ? 3 : 0;
                    if (channels[channel].is_number()) {
                        const int value = static_cast<int>(std::lround(channels[channel].get<double>() * 100.0));
                        light = wxString::Format("%s: %d%%", _L("Light"), value);
                        if (m_light_target != nullptr && wxWindow::FindFocus() != m_light_target)
                            m_light_target->SetValue(value);
                    }
                }
            }
        }
        wxString fan_text;
        int active_fans = 0;
        for (const wxString &fan : fans) {
            if (!fan_text.empty())
                fan_text += "  ";
            fan_text += fan;
            if (!fan.EndsWith(" 0%"))
                ++active_fans;
        }
        if (fans.empty()) {
            m_fans->SetLabel(_L("Fans: -"));
            m_fans->UnsetToolTip();
        } else {
            m_fans->SetLabel(wxString::Format("%s: %d / %zu", _L("Fans"), active_fans, fans.size()));
            m_fans->SetToolTip(fan_text);
        }
        const double speed_factor = status.contains("gcode_move") ? number_or(status.at("gcode_move"), "speed_factor", 1.0) : 1.0;
        const double flow_factor = status.contains("gcode_move") ? number_or(status.at("gcode_move"), "extrude_factor", 1.0) : 1.0;
        m_speed->SetLabel(wxString::Format("%s: %.0f%%", _L("Speed"), speed_factor * 100.0));
        if (m_speed_target != nullptr && wxWindow::FindFocus() != m_speed_target)
            m_speed_target->SetValue(speed_factor * 100.0);
        if (m_flow_target != nullptr && wxWindow::FindFocus() != m_flow_target)
            m_flow_target->SetValue(flow_factor * 100.0);
        m_light->SetLabel(light);

        if (status.contains("bed_mesh")) {
            const json &mesh = status.at("bed_mesh");
            const std::string profile = string_or(mesh, "profile_name");
            const auto matrix = mesh.find("probed_matrix");
            const size_t rows = matrix != mesh.end() && matrix->is_array() ? matrix->size() : 0;
            const size_t columns = rows > 0 && (*matrix)[0].is_array() ? (*matrix)[0].size() : 0;
            m_mesh_status->SetLabel(
                _L("Mesh") + ": " + (profile.empty() ? _L("none") : from_u8(profile)) +
                wxString::Format("  (%zu x %zu)", columns, rows));
        } else {
            m_mesh_status->SetLabel(_L("Mesh: unsupported"));
        }

        if (status.contains("query_endstops") && status.at("query_endstops").is_object()) {
            const json &endstops = status.at("query_endstops");
            const auto last_query = endstops.find("last_query");
            wxString value = _L("Endstops") + ": ";
            if (last_query != endstops.end() && last_query->is_object()) {
                bool first = true;
                for (auto it = last_query->begin(); it != last_query->end(); ++it) {
                    if (!first)
                        value += "  ";
                    first = false;
                    value += from_u8(it.key()) + "=" + (it.value().is_string() ? from_u8(it.value().get<std::string>()) : "-");
                }
            } else {
                value += "-";
            }
            m_endstop_status->SetLabel(value);
        }

        if (status.contains("system_stats") && status.at("system_stats").is_object()) {
            const json &stats = status.at("system_stats");
            const double load = number_or(stats, "sysload");
            const double memory_kib = number_or(stats, "memavail");
            m_network_status->SetToolTip(wxString::Format(
                "CPU load: %.2f\nAvailable memory: %.0f MiB",
                load,
                memory_kib / 1024.0));
        }

        std::string message = string_or(print_stats, "message");
        if (message.empty())
            message = string_or(display, "message");
        m_message->SetLabel(from_u8(message));
        update_controls();
        Layout();
    } catch (const std::exception &e) {
        set_connection_error(from_u8(e.what()));
    }
}

void SnapmakerMonitorPanel::apply_console(const std::string &body)
{
    try {
        const json data = json::parse(body);
        const json &entries = data.at("result").at("gcode_store");
        if (!entries.is_array())
            return;
        wxString output;
        for (const json &entry : entries) {
            if (!entry.is_object())
                continue;
            const std::string type = string_or(entry, "type");
            const std::string message = string_or(entry, "message");
            if (message.empty())
                continue;
            output += wxString::Format("[%s] %s\n", from_u8(type.empty() ? "response" : type), from_u8(message));
        }
        if (m_console_output->GetValue() != output) {
            m_console_output->ChangeValue(output);
            m_console_output->ShowPosition(m_console_output->GetLastPosition());
        }
    } catch (const std::exception &) {
        m_console_output->ChangeValue(_L("Console history unavailable"));
    }
}

void SnapmakerMonitorPanel::apply_files(const std::string &body)
{
    struct FileEntry {
        std::string path;
        double size {0.0};
        double modified {0.0};
    };

    try {
        const json data = json::parse(body);
        const json &files = data.at("result");
        if (!files.is_array())
            return;
        std::vector<FileEntry> entries;
        entries.reserve(files.size());
        for (const json &file : files) {
            if (!file.is_object())
                continue;
            const std::string path = string_or(file, "path");
            if (!path.empty())
                entries.push_back({path, number_or(file, "size"), number_or(file, "modified")});
        }
        std::sort(entries.begin(), entries.end(), [](const FileEntry &left, const FileEntry &right) {
            return left.modified > right.modified;
        });

        std::string selected;
        const int old_selection = m_file_list->GetSelection();
        if (old_selection != wxNOT_FOUND && static_cast<size_t>(old_selection) < m_file_names.size())
            selected = m_file_names[static_cast<size_t>(old_selection)];

        m_file_names.clear();
        m_file_list->Freeze();
        m_file_list->Clear();
        for (const FileEntry &entry : entries) {
            m_file_names.emplace_back(entry.path);
            m_file_list->Append(wxString::Format(
                "%s  (%.1f MiB)",
                from_u8(entry.path),
                entry.size / (1024.0 * 1024.0)));
            if (entry.path == selected)
                m_file_list->SetSelection(static_cast<int>(m_file_names.size() - 1));
        }
        m_file_list->Thaw();
        if (m_file_list->GetSelection() == wxNOT_FOUND)
            m_file_details->SetLabel(_L("Select a file"));
        update_controls();
    } catch (const std::exception &) {
        m_file_details->SetLabel(_L("File list unavailable"));
    }
}

void SnapmakerMonitorPanel::apply_history(const std::string &body)
{
    try {
        const json data = json::parse(body);
        const json &jobs = data.at("result").at("jobs");
        if (!jobs.is_array())
            return;
        m_history_list->Freeze();
        m_history_list->Clear();
        for (const json &job : jobs) {
            if (!job.is_object())
                continue;
            const std::string filename = string_or(job, "filename");
            const std::string state = string_or(job, "status");
            const double duration = number_or(job, "print_duration", number_or(job, "total_duration"));
            m_history_list->Append(
                from_u8(state.empty() ? "unknown" : state) + "  |  " +
                from_u8(filename.empty() ? "-" : filename) + "  |  " +
                duration_text(duration));
        }
        m_history_list->Thaw();
    } catch (const std::exception &) {
        m_history_list->Clear();
        m_history_list->Append(_L("Job history unavailable"));
    }
}

void SnapmakerMonitorPanel::apply_system_info(const std::string &body)
{
    try {
        const json data = json::parse(body);
        const json &system = data.at("result").at("system_info");
        const json &product = system.at("product_info");
        const json &distribution = system.at("distribution");
        const json &cpu = system.at("cpu_info");
        const std::string machine = string_or(product, "machine_type");
        const std::string firmware = string_or(product, "firmware_version");
        const std::string software = string_or(product, "software_version");
        const std::string distro = string_or(distribution, "name");
        const std::string distro_version = string_or(distribution, "version");
        const double total_memory = number_or(cpu, "total_memory");
        m_system_summary->SetLabel(
            _L("Machine") + ": " + from_u8(machine.empty() ? "Snapmaker U1" : machine) + "\n" +
            _L("Firmware") + ": " + from_u8(firmware.empty() ? "-" : firmware) + "\n" +
            _L("Controller") + ": " + from_u8(software.empty() ? "-" : software) + "\n" +
            _L("System") + ": " + from_u8(distro + " " + distro_version) + "\n" +
            wxString::Format("CPU: %.0f cores  |  RAM: %.0f MiB",
                             number_or(cpu, "cpu_count"),
                             total_memory / 1024.0));
        m_system_summary->Wrap(FromDIP(500));
    } catch (const std::exception &) {
        m_system_summary->SetLabel(_L("System information unavailable"));
    }
}

void SnapmakerMonitorPanel::apply_camera(wxImage image, std::uint64_t frame_hash_value, long latency_ms)
{
    m_last_frame_hash = frame_hash_value;
    m_camera_latency_ms = latency_ms;
    m_camera->set_frame(std::move(image));
    ++m_camera_frames_in_window;
    update_camera_rate();
    update_footer();
}

void SnapmakerMonitorPanel::apply_duplicate_camera_frame(long latency_ms)
{
    m_camera_latency_ms = latency_ms;
    update_camera_rate();
    update_footer();
}

void SnapmakerMonitorPanel::set_connection_error(const wxString &message)
{
    m_connected = false;
    m_print_state.clear();
    m_connection->SetLabel(_L("Disconnected"));
    m_connection->SetForegroundColour(wxColour(218, 82, 82));
    m_state->SetLabel(_L("Unavailable"));
    m_message->SetLabel(message);
    m_network_status->SetLabel(_L("LAN: unavailable"));
    update_controls();
}

void SnapmakerMonitorPanel::send_print_command(const std::string &endpoint, const wxString &success_message)
{
    if (m_command_in_flight || !m_connected || m_base_url.empty())
        return;

    m_command_in_flight = true;
    m_message->SetLabel(_L("Sending command..."));
    update_controls();

    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::post(m_base_url + endpoint);
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.header("Content-Length", "0")
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([this, lifetime, generation, success_message](std::string, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, success_message]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                m_message->SetLabel(success_message);
                update_controls();
                request_status();
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                wxString message = _L("Command failed");
                if (!error.empty())
                    message += ": " + from_u8(error);
                m_message->SetLabel(message);
                update_controls();
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::send_delete_command(const std::string &endpoint, const wxString &success_message)
{
    if (m_command_in_flight || !m_connected || m_base_url.empty())
        return;
    m_command_in_flight = true;
    m_message->SetLabel(_L("Sending command..."));
    update_controls();

    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::del(m_base_url + endpoint);
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.timeout_connect(2)
        .timeout_max(8)
        .on_complete([this, lifetime, generation, success_message](std::string, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, success_message]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                m_message->SetLabel(success_message);
                request_files();
                update_controls();
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                m_message->SetLabel(_L("Command failed") + (error.empty() ? wxString() : ": " + from_u8(error)));
                update_controls();
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::send_gcode_script(const std::string &script, const wxString &success_message)
{
    if (m_command_in_flight || !m_connected || m_base_url.empty())
        return;

    m_command_in_flight = true;
    m_message->SetLabel(_L("Sending command..."));
    update_controls();

    const std::string body = json {{"script", script}}.dump();
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::post(m_base_url + "/printer/gcode/script");
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.header("Content-Type", "application/json")
        .header("Content-Length", std::to_string(body.size()))
        .set_post_body(body)
        .timeout_connect(2)
        .timeout_max(180)
        .on_complete([this, lifetime, generation, success_message](std::string, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, success_message]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                m_message->SetLabel(success_message);
                update_controls();
                request_status();
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                wxString message = _L("Command failed");
                if (!error.empty())
                    message += ": " + from_u8(error);
                m_message->SetLabel(message);
                update_controls();
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::refresh_auxiliary()
{
    request_console();
    request_files();
    request_history();
    request_system_info();
}

void SnapmakerMonitorPanel::on_home(const std::string &axes)
{
    std::string script = "G28";
    for (char axis : axes) {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
        if (upper != 'X' && upper != 'Y' && upper != 'Z')
            return;
        script += " ";
        script.push_back(upper);
    }
    send_gcode_script(script, axes.empty() ? _L("Homing complete") : _L("Axis homed"));
}

void SnapmakerMonitorPanel::on_jog(char axis, double direction)
{
    if (!m_homed || m_jog_distance == nullptr)
        return;
    static const std::array<double, 4> distances {0.1, 1.0, 10.0, 50.0};
    const int selection = m_jog_distance->GetSelection();
    if (selection < 0 || static_cast<size_t>(selection) >= distances.size())
        return;
    try {
        send_gcode_script(
            snapmaker_jog_script(axis, direction * distances[static_cast<size_t>(selection)]),
            _L("Move complete"));
    } catch (const std::exception &) {
        m_message->SetLabel(_L("Invalid move"));
    }
}

void SnapmakerMonitorPanel::on_set_temperature(size_t heater_index)
{
    if (heater_index >= m_temperature_targets.size() || m_temperature_targets[heater_index] == nullptr)
        return;
    const std::string heater = heater_index + 1 == m_temperature_targets.size()
        ? "heater_bed"
        : (heater_index == 0 ? "extruder" : "extruder" + std::to_string(heater_index));
    try {
        send_gcode_script(
            snapmaker_heater_script(heater, m_temperature_targets[heater_index]->GetValue()),
            _L("Temperature target updated"));
    } catch (const std::exception &) {
        m_message->SetLabel(_L("Invalid temperature"));
    }
}

void SnapmakerMonitorPanel::on_set_tuning()
{
    if (m_speed_target == nullptr || m_flow_target == nullptr)
        return;
    const int speed = static_cast<int>(std::lround(m_speed_target->GetValue()));
    const int flow = static_cast<int>(std::lround(m_flow_target->GetValue()));
    send_gcode_script(
        "M220 S" + std::to_string(speed) + "\nM221 S" + std::to_string(flow),
        _L("Speed and flow updated"));
}

void SnapmakerMonitorPanel::on_set_fans()
{
    if (m_main_fan_target == nullptr)
        return;
    const int part_fan = static_cast<int>(std::lround(m_main_fan_target->GetValue() * 255.0 / 100.0));
    std::ostringstream script;
    script << "M106 S" << std::clamp(part_fan, 0, 255);
    if (m_cavity_fan_supported && m_cavity_fan_target != nullptr)
        script << "\nSET_FAN_SPEED FAN=cavity_fan SPEED="
               << std::fixed << std::setprecision(2)
               << std::clamp(m_cavity_fan_target->GetValue() / 100.0, 0.0, 1.0);
    if (m_light_supported && m_light_target != nullptr)
        script << "\nSET_LED LED=cavity_led WHITE="
               << std::fixed << std::setprecision(2)
               << std::clamp(m_light_target->GetValue() / 100.0, 0.0, 1.0);
    send_gcode_script(script.str(), _L("Fans and light updated"));
}

void SnapmakerMonitorPanel::on_run_macro()
{
    const int selection = m_macro_list == nullptr ? wxNOT_FOUND : m_macro_list->GetSelection();
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_macro_names.size())
        return;
    const std::string &macro = m_macro_names[static_cast<size_t>(selection)];
    if (!is_public_snapmaker_macro(macro))
        return;
    wxString arguments = m_macro_arguments->GetValue();
    arguments.Trim(true).Trim(false);
    std::string script = macro;
    if (!arguments.empty()) {
        script += " ";
        script += arguments.ToUTF8().data();
    }
    send_gcode_script(script, _L("Macro started") + ": " + from_u8(macro));
}

void SnapmakerMonitorPanel::on_send_console()
{
    if (m_console_command == nullptr)
        return;
    wxString command = m_console_command->GetValue();
    command.Trim(true).Trim(false);
    if (command.empty())
        return;
    m_console_command->Clear();
    send_gcode_script(command.ToUTF8().data(), _L("Command sent"));
}

void SnapmakerMonitorPanel::on_start_file()
{
    const int selection = m_file_list == nullptr ? wxNOT_FOUND : m_file_list->GetSelection();
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_file_names.size() ||
        m_print_state == "printing" || m_print_state == "paused" || m_command_in_flight)
        return;
    const std::string filename = m_file_names[static_cast<size_t>(selection)];
    if (wxMessageBox(
            _L("Check and start this file?") + "\n\n" + from_u8(filename),
            _L("Start print"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxYES)
        return;

    m_command_in_flight = true;
    m_message->SetLabel(_L("Checking pre-print homing..."));
    update_controls();
    const auto lifetime = std::weak_ptr<int>(m_lifetime);
    const std::uint64_t generation = m_server_generation;
    auto request = Slic3r::Http::get(
        m_base_url + "/server/files/gcodes/" + encode_url_path(filename));
    if (!m_api_key.empty())
        request.header("X-API-Key", m_api_key);
    request.header("Range", "bytes=0-524287")
        .timeout_connect(2)
        .timeout_max(15)
        .size_limit(524288)
        .on_complete([this, lifetime, generation, filename](std::string body, unsigned) {
            const bool safe_homing = snapmaker_u1_has_safe_homing(body);
            wxTheApp->CallAfter([this, lifetime, generation, filename, safe_homing]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                if (!safe_homing) {
                    m_message->SetLabel(_L("Print blocked: G28 is missing after PRINT_START"));
                    update_controls();
                    wxMessageBox(
                        _L("This G-code disables the motors in PRINT_START but does not home all axes afterward.")
                            + "\n\n" +
                        _L("Slice the model again with the current Magpie Slicer before printing."),
                        _L("Unsafe G-code"),
                        wxOK | wxICON_ERROR,
                        this);
                    return;
                }
                send_print_command(
                    "/printer/print/start?filename=" + Slic3r::Http::url_encode(filename),
                    _L("Print started"));
            });
        })
        .on_error([this, lifetime, generation](std::string, std::string error, unsigned) {
            wxTheApp->CallAfter([this, lifetime, generation, error = std::move(error)]() {
                if (lifetime.expired() || generation != m_server_generation)
                    return;
                m_command_in_flight = false;
                m_message->SetLabel(_L("Pre-print check failed; print was not started") +
                                    (error.empty() ? wxString() : ": " + from_u8(error)));
                update_controls();
            });
        })
        .perform();
}

void SnapmakerMonitorPanel::on_delete_file()
{
    const int selection = m_file_list == nullptr ? wxNOT_FOUND : m_file_list->GetSelection();
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= m_file_names.size())
        return;
    const std::string filename = m_file_names[static_cast<size_t>(selection)];
    if (wxMessageBox(
            _L("Delete this G-code file?") + "\n\n" + from_u8(filename),
            _L("Delete file"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxYES)
        return;
    send_delete_command(
        "/server/files/gcodes/" + encode_url_path(filename),
        _L("File deleted"));
}

void SnapmakerMonitorPanel::on_emergency_stop()
{
    if (wxMessageBox(
            _L("Immediately stop the printer? Heaters and motion will be disabled."),
            _L("Emergency stop"),
            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR,
            this) != wxYES)
        return;
    send_print_command("/printer/emergency_stop", _L("Emergency stop sent"));
}

void SnapmakerMonitorPanel::update_layer_view(int current_layer, int total_layers)
{
    if (!m_gcode_cache || current_layer <= 0)
        return;
    if (m_displayed_layer == current_layer)
        return;

    const int layer_number =
        valid_snapmaker_layer_number(current_layer, m_gcode_cache->indexed_layer_count());
    if (layer_number == 0) {
        m_layer_view->set_status(wxString::Format(
            "%s: %d / %d",
            _L("G-code layer unavailable"),
            current_layer,
            m_gcode_cache->indexed_layer_count()));
        m_displayed_layer = 0;
        return;
    }

    SnapmakerGCodeLayer layer = m_gcode_cache->layer(layer_number);
    const int layer_count = total_layers > 0 ? total_layers : m_gcode_cache->layer_count();
    m_layer_view->set_layer(std::move(layer), layer_count);
    m_displayed_layer = current_layer;
}

void SnapmakerMonitorPanel::update_object_list(const std::vector<ExcludeObjectShape> &objects)
{
    std::vector<std::string> names;
    std::vector<std::string> signature;
    names.reserve(objects.size());
    signature.reserve(objects.size());
    for (const ExcludeObjectShape &object : objects) {
        names.emplace_back(object.name);
        signature.emplace_back((object.excluded ? "excluded:" : "active:") + object.name);
    }
    if (signature == m_object_list_signature)
        return;

    const std::string selected = m_layer_view == nullptr
        ? std::string()
        : m_layer_view->selected_object();
    m_object_names = std::move(names);
    m_object_list_signature = std::move(signature);
    m_object_list->Freeze();
    m_object_list->Clear();
    for (size_t i = 0; i < objects.size(); ++i) {
        const wxString label = objects[i].excluded
            ? _L("[Skipped]") + " " + from_u8(objects[i].name)
            : from_u8(objects[i].name);
        m_object_list->Append(label);
        if (objects[i].name == selected && !objects[i].excluded)
            m_object_list->SetSelection(static_cast<int>(i));
    }
    m_object_list->Thaw();
}

void SnapmakerMonitorPanel::update_controls()
{
    if (m_pause_resume == nullptr || m_cancel == nullptr || m_refresh == nullptr || m_skip_object == nullptr)
        return;

    const bool object_selected = m_layer_view != nullptr && !m_layer_view->selected_object().empty();
    const SnapmakerControlAvailability availability = snapmaker_control_availability(
        m_connected,
        m_command_in_flight,
        !m_base_url.empty(),
        m_print_state,
        m_exclude_object_supported,
        object_selected);
    m_pause_resume->SetLabel(availability.resume_mode ? _L("Resume") : _L("Pause"));
    m_pause_resume->Enable(availability.pause_resume);
    m_cancel->Enable(availability.cancel);
    m_refresh->Enable(availability.refresh);
    m_skip_object->Enable(availability.skip_object);
    const bool command_ready = m_connected && !m_command_in_flight;
    for (Button *button : m_connected_buttons)
        if (button != nullptr)
            button->Enable(command_ready);
    for (Button *button : m_jog_buttons)
        if (button != nullptr)
            button->Enable(command_ready && m_homed);

    const int macro_selection = m_macro_list == nullptr ? wxNOT_FOUND : m_macro_list->GetSelection();
    if (m_macro_run != nullptr)
        m_macro_run->Enable(command_ready && macro_selection != wxNOT_FOUND);
    if (m_console_send != nullptr)
        m_console_send->Enable(command_ready);

    const int file_selection = m_file_list == nullptr ? wxNOT_FOUND : m_file_list->GetSelection();
    const bool file_selected =
        file_selection != wxNOT_FOUND && static_cast<size_t>(file_selection) < m_file_names.size();
    const bool printer_idle = m_print_state != "printing" && m_print_state != "paused";
    if (m_file_print != nullptr)
        m_file_print->Enable(command_ready && printer_idle && file_selected);
    if (m_file_delete != nullptr)
        m_file_delete->Enable(command_ready && printer_idle && file_selected);
    if (m_mesh_calibrate != nullptr)
        m_mesh_calibrate->Enable(command_ready && printer_idle && m_bed_mesh_supported);
    if (m_mesh_clear != nullptr)
        m_mesh_clear->Enable(command_ready && m_bed_mesh_supported);
    if (m_cavity_fan_target != nullptr)
        m_cavity_fan_target->Enable(command_ready && m_cavity_fan_supported);
    if (m_light_target != nullptr)
        m_light_target->Enable(command_ready && m_light_supported);
}

void SnapmakerMonitorPanel::on_pause_resume()
{
    if (m_print_state == "paused")
        send_print_command("/printer/print/resume", _L("Print resumed"));
    else if (m_print_state == "printing")
        send_print_command("/printer/print/pause", _L("Print paused"));
}

void SnapmakerMonitorPanel::on_cancel()
{
    if (m_print_state != "printing" && m_print_state != "paused")
        return;
    if (wxMessageBox(
            _L("Cancel the current print?"),
            _L("Cancel print"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxYES)
        return;
    send_print_command("/printer/print/cancel", _L("Print cancelled"));
}

void SnapmakerMonitorPanel::on_skip_object()
{
    if (!m_exclude_object_supported || m_layer_view == nullptr)
        return;
    const std::string object_name = m_layer_view->selected_object();
    if (!is_valid_snapmaker_object_name(object_name))
        return;

    const wxString question = _L("Skip this model for the rest of the print?") + "\n\n" + from_u8(object_name);
    if (wxMessageBox(
            question,
            _L("Skip selected model"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxYES)
        return;

    send_gcode_script(
        "EXCLUDE_OBJECT NAME=" + object_name,
        _L("Model skipped") + ": " + from_u8(object_name));
}

void SnapmakerMonitorPanel::update_camera_rate()
{
    const auto now = std::chrono::steady_clock::now();
    const double window_seconds = std::chrono::duration<double>(now - m_fps_window_started).count();
    if (window_seconds >= 1.0) {
        m_camera_fps = m_camera_frames_in_window / window_seconds;
        m_camera_frames_in_window = 0;
        m_fps_window_started = now;
    }
}

void SnapmakerMonitorPanel::update_footer()
{
    if (m_camera_fps > 0.0) {
        m_camera_status->SetLabel(wxString::Format(
            "%s: %.1f FPS  |  %s: %.1f Hz",
            _L("Camera"),
            m_camera_fps,
            _L("Polling"),
            1000.0 / m_camera_interval_ms));
    } else {
        m_camera_status->SetLabel(
            m_camera_session_status.empty() ? _L("Camera: waiting") : m_camera_session_status);
    }
    if (m_camera_latency_ms > 0)
        m_network_status->SetLabel(wxString::Format("LAN: %ld ms", m_camera_latency_ms));
    else
        m_network_status->SetLabel(_L("LAN: -"));
}

void SnapmakerMonitorPanel::on_status_timer(wxTimerEvent &)
{
    if (m_status_objects.empty())
        request_object_list();
    else
        request_status();
    ++m_slow_refresh_tick;
    if (m_slow_refresh_tick % 5 == 0)
        request_console();
    if (m_slow_refresh_tick % 15 == 0) {
        request_files();
        request_history();
        request_system_info();
    }
}

void SnapmakerMonitorPanel::on_camera_timer(wxTimerEvent &)
{
    if (!m_active || !IsShownOnScreen())
        return;
    if (auto *window = dynamic_cast<wxTopLevelWindow *>(wxGetTopLevelParent(this)); window != nullptr && window->IsIconized())
        return;
    request_camera();
}

std::string SnapmakerMonitorPanel::make_status_url() const
{
    std::ostringstream url;
    url << m_base_url << "/printer/objects/query?";
    for (size_t i = 0; i < m_status_objects.size(); ++i) {
        if (i != 0)
            url << '&';
        url << Slic3r::Http::url_encode(m_status_objects[i]);
    }
    return url.str();
}

std::string SnapmakerMonitorPanel::normalize_base_url(const wxString &url)
{
    return normalize_snapmaker_base_url(url.ToUTF8().data());
}

} // namespace GUI
} // namespace Slic3r
