#ifndef slic3r_GUI_SnapmakerCameraSession_hpp_
#define slic3r_GUI_SnapmakerCameraSession_hpp_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {
namespace GUI {

class SnapmakerCameraSession final
{
public:
    enum class State {
        Starting,
        Active,
        Reconnecting,
        Stopped,
        Error
    };

    using StateCallback = std::function<void(State, const std::string &)>;

    SnapmakerCameraSession() = default;
    ~SnapmakerCameraSession();

    SnapmakerCameraSession(const SnapmakerCameraSession &) = delete;
    SnapmakerCameraSession &operator=(const SnapmakerCameraSession &) = delete;

    void start(std::string base_url, std::string api_key, StateCallback callback);
    void stop();
    bool is_running() const { return m_running.load(); }

private:
    void run(std::string base_url, std::string api_key, StateCallback callback);
    bool wait_for_stop(std::chrono::milliseconds duration);

    std::atomic_bool m_stop_requested {false};
    std::atomic_bool m_running {false};
    std::mutex m_wait_mutex;
    std::condition_variable m_wait_condition;
    std::thread m_thread;
};

} // namespace GUI
} // namespace Slic3r

#endif
