#include "SnapmakerCameraSession.hpp"
#include "SnapmakerMonitorUtils.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

namespace {

namespace beast = boost::beast;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(5);
constexpr auto READ_TIMEOUT = std::chrono::milliseconds(500);
constexpr auto KEEPALIVE_INTERVAL = std::chrono::seconds(10);
constexpr auto MAX_RETRY_DELAY = std::chrono::seconds(5);

json make_identify_request()
{
    return {
        {"jsonrpc", "2.0"},
        {"method", "server.connection.identify"},
        {"params", {
            {"client_name", "MagpieSlicer"},
            {"version", "2.5.0"},
            {"type", "agent"},
            {"url", "https://github.com"}
        }},
        {"id", 1}
    };
}

json make_camera_request(const char *method, int id)
{
    json params = {{"domain", "lan"}};
    if (std::string(method) == "camera.start_monitor") {
        params["interval"] = 0;
        params["expect_pw"] = true;
    }
    return {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
        {"id", id}
    };
}

} // namespace

SnapmakerCameraSession::~SnapmakerCameraSession()
{
    stop();
}

void SnapmakerCameraSession::start(std::string base_url, std::string api_key, StateCallback callback)
{
    stop();
    if (base_url.empty())
        return;

    m_stop_requested.store(false);
    m_running.store(true);
    m_thread = std::thread(
        [this, base_url = std::move(base_url), api_key = std::move(api_key), callback = std::move(callback)]() mutable {
            run(std::move(base_url), std::move(api_key), std::move(callback));
        });
}

void SnapmakerCameraSession::stop()
{
    m_stop_requested.store(true);
    m_wait_condition.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

bool SnapmakerCameraSession::wait_for_stop(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(m_wait_mutex);
    return m_wait_condition.wait_for(lock, duration, [this]() { return m_stop_requested.load(); });
}

void SnapmakerCameraSession::run(std::string base_url, std::string api_key, StateCallback callback)
{
    auto notify = [&callback](State state, const std::string &message = std::string()) {
        if (callback)
            callback(state, message);
    };

    try {
        const SnapmakerWebSocketEndpoint endpoint =
            parse_snapmaker_websocket_endpoint(std::move(base_url));
        unsigned retry_count = 0;

        while (!m_stop_requested.load()) {
            notify(retry_count == 0 ? State::Starting : State::Reconnecting);
            try {
                net::io_context ioc;
                tcp::resolver resolver(ioc);
                beast::tcp_stream stream(ioc);
                stream.expires_after(CONNECT_TIMEOUT);
                stream.connect(resolver.resolve(endpoint.host, endpoint.port));

                websocket::stream<beast::tcp_stream> ws(std::move(stream));
                ws.set_option(websocket::stream_base::decorator([&api_key](websocket::request_type &request) {
                    request.set(beast::http::field::user_agent, "MagpieSlicer/2.5.0");
                    if (!api_key.empty())
                        request.set("X-Api-Key", api_key);
                }));
                ws.handshake(endpoint.host_header, endpoint.target);
                ws.text(true);
                ws.write(net::buffer(make_identify_request().dump()));
                ws.write(net::buffer(make_camera_request("camera.start_monitor", 2).dump()));

                retry_count = 0;
                notify(State::Active);
                auto last_keepalive = std::chrono::steady_clock::now();

                while (!m_stop_requested.load()) {
                    ws.next_layer().expires_after(READ_TIMEOUT);
                    beast::flat_buffer buffer;
                    beast::error_code error;
                    ws.read(buffer, error);
                    if (error == beast::error::timeout) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_keepalive >= KEEPALIVE_INTERVAL) {
                            ws.ping(websocket::ping_data {}, error);
                            if (error)
                                throw beast::system_error(error);
                            last_keepalive = now;
                        }
                        continue;
                    }
                    if (error == websocket::error::closed)
                        throw std::runtime_error("Snapmaker camera websocket closed");
                    if (error)
                        throw beast::system_error(error);
                }

                beast::error_code error;
                ws.write(net::buffer(make_camera_request("camera.stop_monitor", 3).dump()), error);
                ws.close(websocket::close_code::normal, error);
                break;
            } catch (const std::exception &e) {
                if (m_stop_requested.load())
                    break;
                ++retry_count;
                BOOST_LOG_TRIVIAL(warning) << "Snapmaker camera session reconnecting: " << e.what();
                notify(State::Reconnecting, e.what());
                const auto retry_delay = std::min(MAX_RETRY_DELAY, std::chrono::seconds(retry_count));
                if (wait_for_stop(std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay)))
                    break;
            }
        }
        notify(State::Stopped);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Snapmaker camera session failed: " << e.what();
        notify(State::Error, e.what());
    }
    m_running.store(false);
}

} // namespace GUI
} // namespace Slic3r
