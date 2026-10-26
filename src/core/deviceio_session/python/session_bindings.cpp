// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <deviceio_base/tracker_vendor.hpp>
#include <deviceio_py_utils/session.hpp>
#include <deviceio_session/deviceio_session.hpp>
#include <deviceio_session/remote_session_client.hpp>
#include <deviceio_session/replay_session.hpp>
#include <openxr/openxr.h>
#include <pybind11/stl.h>
#include <remote_link/reject.hpp>
#include <remote_link/tcp_transport.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

PYBIND11_MODULE(_deviceio_session, m)
{
    m.doc() = "Isaac Teleop DeviceIO - Session management";

    py::module_::import("isaaccapture.deviceio_trackers._deviceio_trackers");

    py::register_local_exception_translator(
        [](std::exception_ptr p)
        {
            try
            {
                if (p)
                {
                    std::rethrow_exception(p);
                }
            }
            catch (const core::RemoteConnectTimeoutError& e)
            {
                py::set_error(PyExc_TimeoutError, e.what());
            }
        });

    // ---- McapRecordingConfig (live recording) ----
    py::class_<core::McapRecordingConfig>(m, "McapRecordingConfig",
                                          "Configuration for MCAP recording. "
                                          "Pass to DeviceIOSession.run() to enable recording, "
                                          "or omit / pass None to disable.")
        .def(py::init(
                 [](const std::string& filename,
                    const std::vector<std::pair<std::shared_ptr<core::ITracker>, std::string>>& tracker_names)
                 {
                     core::McapRecordingConfig config;
                     config.filename = filename;
                     for (const auto& [tracker, name] : tracker_names)
                     {
                         config.tracker_names.emplace_back(tracker.get(), name);
                     }
                     return config;
                 }),
             py::arg("filename"),
             py::arg("tracker_names") = std::vector<std::pair<std::shared_ptr<core::ITracker>, std::string>>{})
        .def_readwrite("filename", &core::McapRecordingConfig::filename)
        .def(
            "get_tracker_names",
            [](const core::McapRecordingConfig& c)
            {
                py::list result;
                for (const auto& [tracker, name] : c.tracker_names)
                {
                    result.append(py::make_tuple(py::cast(tracker), name));
                }
                return result;
            },
            "Return the list of (tracker, channel_name) pairs.");

    // ---- McapReplayConfig (replay) ----
    py::class_<core::McapReplayConfig>(m, "McapReplayConfig",
                                       "Configuration for MCAP replay sessions. "
                                       "Pass to ReplaySession.run() to create a replay session.")
        .def(py::init(
                 [](const std::string& filename,
                    const std::vector<std::pair<std::shared_ptr<core::ITracker>, std::string>>& tracker_names)
                 {
                     core::McapReplayConfig config;
                     config.filename = filename;
                     for (const auto& [tracker, name] : tracker_names)
                     {
                         config.tracker_names.emplace_back(tracker.get(), name);
                     }
                     return config;
                 }),
             py::arg("filename"),
             py::arg("tracker_names") = std::vector<std::pair<std::shared_ptr<core::ITracker>, std::string>>{})
        .def_readwrite("filename", &core::McapReplayConfig::filename)
        .def(
            "get_tracker_names",
            [](const core::McapReplayConfig& c)
            {
                py::list result;
                for (const auto& [tracker, name] : c.tracker_names)
                {
                    result.append(py::make_tuple(py::cast(tracker), name));
                }
                return result;
            },
            "Return the list of (tracker, channel_name) pairs.");

    // ---- TrackerVendor / VendorConfig (live vendor selection) ----
    py::class_<core::TrackerVendor>(m, "TrackerVendor",
                                    "Per-tracker vendor selection: a string id (e.g. \"body.pico-xr\") plus "
                                    "free-form vendor params. Pass inside VendorConfig to DeviceIOSession.run().")
        .def(py::init(
                 [](std::string id, std::map<std::string, std::string> params)
                 {
                     core::TrackerVendor vendor;
                     vendor.id = std::move(id);
                     vendor.params = std::move(params);
                     return vendor;
                 }),
             py::arg("id"), py::arg("params") = std::map<std::string, std::string>{})
        .def_readwrite("id", &core::TrackerVendor::id)
        .def_readwrite("params", &core::TrackerVendor::params);

    py::class_<core::VendorConfig>(m, "VendorConfig",
                                   "Per-session vendor selection for vendored trackers (live sessions only). "
                                   "Trackers not listed use their default vendor id.")
        .def(py::init(
                 [](const std::vector<std::pair<std::shared_ptr<core::ITracker>, core::TrackerVendor>>& tracker_vendors)
                 {
                     core::VendorConfig config;
                     for (const auto& [tracker, vendor] : tracker_vendors)
                     {
                         config.tracker_vendors.emplace_back(tracker.get(), vendor);
                     }
                     return config;
                 }),
             py::arg("tracker_vendors") = std::vector<std::pair<std::shared_ptr<core::ITracker>, core::TrackerVendor>>{})
        .def(
            "get_tracker_vendors",
            [](const core::VendorConfig& c)
            {
                py::list result;
                for (const auto& [tracker, vendor] : c.tracker_vendors)
                {
                    result.append(py::make_tuple(py::cast(tracker), vendor));
                }
                return result;
            },
            "Return the list of (tracker, TrackerVendor) pairs.");

    // ---- DeviceIOSession (live) ----
    py::class_<core::PyDeviceIOSession, core::ITrackerSession, std::unique_ptr<core::PyDeviceIOSession>>(
        m, "DeviceIOSession")
        .def("update", &core::PyDeviceIOSession::update, "Update session and all trackers")
        .def("close", &core::PyDeviceIOSession::close,
             "Release the native session immediately (usually automatic via context manager)")
        .def("__enter__", &core::PyDeviceIOSession::enter)
        .def("__exit__", &core::PyDeviceIOSession::exit)
        .def_static("get_required_extensions", &core::DeviceIOSession::get_required_extensions, py::arg("trackers"),
                    py::arg("vendor_config") = core::VendorConfig{},
                    "Aggregate OpenXR extensions required for a live session with these tracker types "
                    "(not a per-tracker instance method). Pass a VendorConfig to resolve vendored trackers.")
        .def_static(
            "run",
            [](const std::vector<std::shared_ptr<core::ITracker>>& trackers, const core::OpenXRSessionHandles& handles,
               std::optional<core::McapRecordingConfig> recording_config, core::VendorConfig vendor_config)
            {
                if (handles.instance == XR_NULL_HANDLE || handles.session == XR_NULL_HANDLE ||
                    handles.space == XR_NULL_HANDLE || handles.xrGetInstanceProcAddr == nullptr)
                {
                    throw std::runtime_error(
                        "DeviceIOSession.run: invalid OpenXRSessionHandles (instance, session, space must be non-null "
                        "handles and xrGetInstanceProcAddr must be set)");
                }
                auto session =
                    core::DeviceIOSession::run(trackers, handles, std::move(recording_config), std::move(vendor_config));
                return std::make_unique<core::PyDeviceIOSession>(std::move(session));
            },
            py::arg("trackers"), py::arg("handles"), py::arg("recording_config") = py::none(),
            py::arg("vendor_config") = core::VendorConfig{},
            "Create and initialize a session with trackers. "
            "Pass a McapRecordingConfig to enable MCAP recording, and a VendorConfig to select "
            "vendors for any vendored trackers.");

    // ---- ReplaySession ----
    py::class_<core::PyReplaySession, core::ITrackerSession, std::unique_ptr<core::PyReplaySession>>(m, "ReplaySession")
        .def("update", &core::PyReplaySession::update, "Advance replay by one frame")
        .def("close", &core::PyReplaySession::close,
             "Release the native session immediately (usually automatic via context manager)")
        .def("__enter__", &core::PyReplaySession::enter)
        .def("__exit__", &core::PyReplaySession::exit)
        .def_static(
            "run",
            [](const core::McapReplayConfig& config)
            {
                auto session = core::ReplaySession::run(config);
                return std::make_unique<core::PyReplaySession>(std::move(session));
            },
            py::arg("config"),
            "Create a replay session that reads recorded data from an MCAP file. "
            "The McapReplayConfig.tracker_names maps tracker objects to their MCAP channel base names.");

    // ---- Remote sessions ----
    // Only the blocking calls release the GIL; the C++ threads behind them never take it.
    py::class_<core::PyRemoteSession, core::ITrackerSession, std::unique_ptr<core::PyRemoteSession>>(
        m, "RemoteSession",
        "Robot end of a remote session: reads tracker data that a Teleop Session Server records. "
        "create() opens it without any network I/O; connect() then joins a server.")
        .def_static("create", &core::PyRemoteSession::create, py::arg("trackers"),
                    py::arg("max_frame_age_ns") = int64_t{ 250'000'000 },
                    "Create a session for (tracker, name) pairs; the server must record each tracker under the "
                    "same name. update() publishes no data once the newest frame is older than max_frame_age_ns.")
        .def("subscribed_topics", &core::PyRemoteSession::subscribed_topics,
             "The record topics the trackers read, sorted; the server records only these.")
        .def("connect", &core::PyRemoteSession::connect, py::arg("endpoint"), py::arg("hello"), py::arg("timeout_ms"),
             "Connect to endpoint ('host:port', 'tcp://host:port' or 'ws://host:port[/path]'), send the HELLO JSON "
             "and wait for HELLO_ACK. A negative timeout waits without limit. Raises TimeoutError without "
             "HELLO_ACK in time, RuntimeError with the server's ERROR text or when the connection fails. "
             "Releases the GIL.")
        .def("wait_ready", &core::PyRemoteSession::wait_ready, py::arg("timeout_ms"),
             "True once the server reported READY, False after timeout_ms (negative: no limit). Raises "
             "RuntimeError once the session failed. Releases the GIL.")
        .def("latest_status", &core::PyRemoteSession::latest_status,
             "The newest STATUS or READY JSON from the server, or None.")
        .def("hello_ack", &core::PyRemoteSession::hello_ack, "The server's HELLO_ACK JSON ('' before connect()).")
        .def("update", &core::PyRemoteSession::update,
             "Publish the newest received frame to every tracker. Never waits for data. Raises RuntimeError "
             "with the server's ERROR text, or once the link is lost.")
        .def_property_readonly(
            "is_fresh", &core::PyRemoteSession::is_fresh, "Whether the last update() published a fresh frame.")
        .def_property_readonly("frame_age_ns", &core::PyRemoteSession::frame_age_ns,
                               "Age of the frame the last update() published, or None when it published none.")
        .def("close", &core::PyRemoteSession::close,
             "Disconnect and release the native session (usually automatic via context manager). Releases the GIL.")
        .def("__enter__", &core::PyRemoteSession::enter)
        .def("__exit__", &core::PyRemoteSession::exit);

    py::class_<core::PyRemoteSessionHost, std::unique_ptr<core::PyRemoteSessionHost>>(
        m, "RemoteSessionHost",
        "Server end of a remote session: records a DeviceIOSession and streams it to one robot app. "
        "Call start_session(), tick() and close() from one thread at a time.")
        .def_static("adopt", &core::PyRemoteSessionHost::adopt, py::arg("fd"), py::arg("websocket"),
                    "Take ownership of an accepted socket descriptor and start the link.")
        .def("wait_hello", &core::PyRemoteSessionHost::wait_hello, py::arg("timeout_ms"),
             "The robot's HELLO JSON, or None after timeout_ms (negative: no limit). Raises RuntimeError once "
             "the link is lost. Releases the GIL.")
        .def("send_hello_ack", &core::PyRemoteSessionHost::send_hello_ack, py::arg("json"))
        .def("send_status", &core::PyRemoteSessionHost::send_status, py::arg("json"))
        .def("send_ready", &core::PyRemoteSessionHost::send_ready, py::arg("json"))
        .def("send_error", &core::PyRemoteSessionHost::send_error, py::arg("text"),
             "Send the error that ends the session; close() delivers it before closing the connection.")
        .def(
            "start_session",
            [](core::PyRemoteSessionHost& self, const core::PyRemoteSession::TrackerNames& trackers,
               const core::OpenXRSessionHandles& handles, const core::VendorConfig& vendor_config,
               const std::vector<std::string>& topics)
            {
                if (handles.instance == XR_NULL_HANDLE || handles.session == XR_NULL_HANDLE ||
                    handles.space == XR_NULL_HANDLE || handles.xrGetInstanceProcAddr == nullptr)
                {
                    throw std::runtime_error(
                        "RemoteSessionHost.start_session: invalid OpenXRSessionHandles (instance, session, space "
                        "must be non-null handles and xrGetInstanceProcAddr must be set)");
                }
                self.start_session(trackers, handles, vendor_config, topics);
            },
            py::arg("trackers"), py::arg("handles"), py::arg("vendor_config"), py::arg("topics"),
            "Create the DeviceIOSession, recording each (tracker, name) pair; only `topics` are sent "
            "(empty: all). Releases the GIL.")
        .def("tick", &core::PyRemoteSessionHost::tick,
             "Update the DeviceIOSession once and send the result as one frame. Releases the GIL.")
        .def_property_readonly("alive", &core::PyRemoteSessionHost::alive, "False once the link is down.")
        .def_property_readonly(
            "close_reason", &core::PyRemoteSessionHost::close_reason, "Why the link went down ('' while alive).")
        .def("close", &core::PyRemoteSessionHost::close,
             "End the DeviceIOSession, deliver queued messages and close the connection. Idempotent. "
             "Releases the GIL.")
        .def("__enter__", &core::PyRemoteSessionHost::enter)
        .def("__exit__", &core::PyRemoteSessionHost::exit);

    py::class_<core::Listener, std::unique_ptr<core::Listener>>(
        m, "RemoteSessionListener", "Listening TCP socket for Teleop Session Server connections.")
        .def(py::init(
                 [](const std::string& host, int port)
                 {
                     if (port < 0 || port > 65535)
                     {
                         throw py::value_error("RemoteSessionListener: port must be in [0, 65535]");
                     }
                     return std::make_unique<core::Listener>(core::Endpoint{ host, static_cast<uint16_t>(port) });
                 }),
             py::arg("host"), py::arg("port"), "Bind and listen; port 0 picks a free port.")
        .def_property_readonly("port", &core::Listener::port, "The bound port.")
        .def(
            "accept",
            [](core::Listener& self, int64_t timeout_ms) -> std::optional<std::pair<int, std::string>>
            {
                const std::optional<core::Listener::Accepted> accepted =
                    self.accept(std::chrono::milliseconds(timeout_ms));
                if (!accepted)
                {
                    return std::nullopt;
                }
                return std::make_pair(accepted->fd, accepted->peer);
            },
            py::arg("timeout_ms"), py::call_guard<py::gil_scoped_release>(),
            "(fd, peer) of the next connection, or None after timeout_ms (negative: no limit). The caller owns "
            "fd. Releases the GIL.")
        .def_static(
            "reject",
            [](int fd, bool websocket, const std::string& message)
            { core::reject_remote_connection(fd, websocket, message); },
            py::arg("fd"), py::arg("websocket"), py::arg("message"), py::call_guard<py::gil_scoped_release>(),
            "Refuse an accepted connection with an ERROR carrying `message`, then close fd (blocks up to about "
            "3 s). Releases the GIL.");
}
