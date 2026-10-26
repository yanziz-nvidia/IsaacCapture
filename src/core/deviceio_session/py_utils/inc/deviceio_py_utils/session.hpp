// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deviceio_session/deviceio_session.hpp>
#include <deviceio_session/remote_session.hpp>
#include <deviceio_session/remote_session_client.hpp>
#include <deviceio_session/remote_session_host.hpp>
#include <deviceio_session/replay_session.hpp>
#include <mcap/record_stream.hpp>
#include <pybind11/pybind11.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace core
{

//! Whether a destructor may release the GIL around a blocking teardown: pybind runs it with the GIL held, and a
//! finalizing interpreter must keep it.
inline bool can_release_gil()
{
    return Py_IsInitialized() && PyGILState_Check();
}

/**
 * @brief Python-facing session wrapper: destroys the underlying DeviceIOSession in __exit__.
 *
 * Binding DeviceIOSession directly with a no-op __exit__ leaves destruction to the pybind
 * holder, which can run after the OpenXR session is torn down and produces invalid-handle
 * errors. This type inherits ITrackerSession and forwards get_tracker_impl() so tracker
 * accessors and MCAP record() accept the same Python object.
 */
class PyDeviceIOSession : public ITrackerSession
{
public:
    explicit PyDeviceIOSession(std::unique_ptr<DeviceIOSession> impl) : impl_(std::move(impl))
    {
    }

    void update()
    {
        if (!impl_)
        {
            throw std::runtime_error("Session has been closed/destroyed");
        }
        impl_->update();
    }

    void close()
    {
        impl_.reset();
    }

    PyDeviceIOSession& enter()
    {
        if (!impl_)
        {
            throw std::runtime_error("Session has been closed/destroyed");
        }
        return *this;
    }

    void exit(py::object, py::object, py::object)
    {
        close();
    }

    DeviceIOSession& native()
    {
        if (!impl_)
        {
            throw std::runtime_error("Session has been closed/destroyed");
        }
        return *impl_;
    }

    const ITrackerImpl& get_tracker_impl(const ITracker& tracker) const override
    {
        if (!impl_)
        {
            throw std::runtime_error("Session has been closed/destroyed");
        }
        return impl_->get_tracker_impl(tracker);
    }

private:
    std::unique_ptr<DeviceIOSession> impl_;
};

/**
 * @brief Python-facing wrapper for ReplaySession with the same context-manager
 *        and lifetime semantics as PyDeviceIOSession.
 */
class PyReplaySession : public ITrackerSession
{
public:
    explicit PyReplaySession(std::unique_ptr<ReplaySession> impl) : impl_(std::move(impl))
    {
    }

    void update()
    {
        if (!impl_)
        {
            throw std::runtime_error("ReplaySession has been closed/destroyed");
        }
        impl_->update();
    }

    void close()
    {
        impl_.reset();
    }

    PyReplaySession& enter()
    {
        if (!impl_)
        {
            throw std::runtime_error("ReplaySession has been closed/destroyed");
        }
        return *this;
    }

    void exit(py::object, py::object, py::object)
    {
        close();
    }

    const ITrackerImpl& get_tracker_impl(const ITracker& tracker) const override
    {
        if (!impl_)
        {
            throw std::runtime_error("ReplaySession has been closed/destroyed");
        }
        return impl_->get_tracker_impl(tracker);
    }

private:
    std::unique_ptr<ReplaySession> impl_;
};

/**
 * @brief Python-facing RemoteSession: the record stream, the RemoteSession reading it and, after connect(), the
 *        RemoteSessionClient feeding it. Same context-manager semantics as PyDeviceIOSession.
 *
 * connect(), wait_ready(), close() and the destructor release the GIL for their blocking part. Members change only
 * while the GIL is held, and a released call keeps its own reference to the client, so close() on another thread
 * cannot free it.
 */
class PyRemoteSession : public ITrackerSession
{
public:
    using TrackerNames = std::vector<std::pair<std::shared_ptr<ITracker>, std::string>>;

    ~PyRemoteSession() override
    {
        if (can_release_gil())
        {
            close();
        }
    }

    static std::unique_ptr<PyRemoteSession> create(const TrackerNames& trackers, int64_t max_frame_age_ns)
    {
        if (max_frame_age_ns <= 0)
        {
            throw std::invalid_argument("RemoteSession: max_frame_age_ns must be positive");
        }
        std::unique_ptr<PyRemoteSession> self(new PyRemoteSession());
        RemoteSessionConfig config;
        config.stream = self->stream_;
        config.max_frame_age_ns = max_frame_age_ns;
        for (const auto& [tracker, name] : trackers)
        {
            config.tracker_names.emplace_back(tracker.get(), name);
            self->trackers_.push_back(tracker);
        }
        self->session_ = RemoteSession::run(std::move(config));
        return self;
    }

    std::vector<std::string> subscribed_topics() const
    {
        check_open();
        return stream_->subscribed_topics();
    }

    void connect(const std::string& endpoint, std::string hello_json, int64_t timeout_ms)
    {
        check_open();
        if (client_ || connecting_)
        {
            throw std::runtime_error("RemoteSession is already connected");
        }
        connecting_ = true;
        std::shared_ptr<McapRecordStreamReader> stream = stream_;
        std::unique_ptr<RemoteSessionClient> client;
        try
        {
            py::gil_scoped_release release;
            client = RemoteSessionClient::connect(
                endpoint, std::move(hello_json), std::move(stream), std::chrono::milliseconds(timeout_ms));
        }
        catch (...)
        {
            connecting_ = false;
            throw;
        }
        connecting_ = false;
        if (!session_)
        {
            // close() ran while connecting.
            py::gil_scoped_release release;
            client.reset();
            throw std::runtime_error("RemoteSession has been closed/destroyed");
        }
        client_ = std::move(client);
    }

    bool wait_ready(int64_t timeout_ms)
    {
        std::shared_ptr<RemoteSessionClient> client = connected_client();
        py::gil_scoped_release release;
        // Declared after the release, so a last reference is dropped without the GIL.
        const std::shared_ptr<RemoteSessionClient> held = std::move(client);
        return held->wait_ready(std::chrono::milliseconds(timeout_ms)) == RemoteReadyState::Ready;
    }

    std::optional<std::string> latest_status() const
    {
        return client_ ? client_->latest_status() : std::nullopt;
    }

    std::string hello_ack() const
    {
        return client_ ? client_->hello_ack() : std::string();
    }

    void update()
    {
        connected_client()->throw_if_failed();
        session_->update();
    }

    bool is_fresh() const
    {
        check_open();
        return session_->is_fresh();
    }

    std::optional<int64_t> frame_age_ns() const
    {
        check_open();
        return session_->frame_age_ns();
    }

    void close()
    {
        std::shared_ptr<RemoteSessionClient> client = std::exchange(client_, nullptr);
        std::unique_ptr<RemoteSession> session = std::exchange(session_, nullptr);
        // Declared before the release, so they go after the session, with the GIL held again.
        std::shared_ptr<McapRecordStreamReader> stream = std::exchange(stream_, nullptr);
        std::vector<std::shared_ptr<ITracker>> trackers = std::exchange(trackers_, {});
        py::gil_scoped_release release;
        if (client)
        {
            // Also ends a wait_ready() that holds its own reference on another thread.
            client->close();
        }
        client.reset();
        session.reset();
    }

    PyRemoteSession& enter()
    {
        check_open();
        return *this;
    }

    void exit(py::object, py::object, py::object)
    {
        close();
    }

    const ITrackerImpl& get_tracker_impl(const ITracker& tracker) const override
    {
        check_open();
        return session_->get_tracker_impl(tracker);
    }

private:
    PyRemoteSession() = default;

    void check_open() const
    {
        if (!session_)
        {
            throw std::runtime_error("RemoteSession has been closed/destroyed");
        }
    }

    std::shared_ptr<RemoteSessionClient> connected_client() const
    {
        check_open();
        if (!client_)
        {
            throw std::runtime_error("RemoteSession is not connected; call connect() first");
        }
        return client_;
    }

    // Destroyed in reverse order: the client stops feeding the stream before the session and trackers go.
    std::vector<std::shared_ptr<ITracker>> trackers_;
    std::shared_ptr<McapRecordStreamReader> stream_ = std::make_shared<McapRecordStreamReader>();
    std::unique_ptr<RemoteSession> session_;
    std::shared_ptr<RemoteSessionClient> client_;
    bool connecting_ = false;
};

/**
 * @brief Python-facing RemoteSessionHost.
 *
 * wait_hello(), start_session(), tick(), close() and the destructor release the GIL. A mutex keeps start_session(),
 * tick() and close() from overlapping, as RemoteSessionHost requires; it is only ever waited for without the GIL or
 * held without waiting for the GIL, so the two cannot deadlock.
 */
class PyRemoteSessionHost
{
public:
    // Closing lingers until the robot closes its end (up to about 1.5 s).
    ~PyRemoteSessionHost()
    {
        if (can_release_gil())
        {
            close();
        }
    }

    static std::unique_ptr<PyRemoteSessionHost> adopt(int fd, bool websocket)
    {
        return std::unique_ptr<PyRemoteSessionHost>(new PyRemoteSessionHost(RemoteSessionHost::adopt(fd, websocket)));
    }

    std::optional<std::string> wait_hello(int64_t timeout_ms)
    {
        py::gil_scoped_release release;
        return host_->wait_hello(std::chrono::milliseconds(timeout_ms));
    }

    void send_hello_ack(std::string json)
    {
        host_->send_hello_ack(std::move(json));
    }

    void send_status(std::string json)
    {
        host_->send_status(std::move(json));
    }

    void send_ready(std::string json)
    {
        host_->send_ready(std::move(json));
    }

    void send_error(std::string text)
    {
        host_->send_error(std::move(text));
    }

    void start_session(const PyRemoteSession::TrackerNames& trackers,
                       const OpenXRSessionHandles& handles,
                       const VendorConfig& vendor_config,
                       const std::vector<std::string>& topics)
    {
        std::vector<std::shared_ptr<ITracker>> tracker_list;
        std::vector<std::pair<const ITracker*, std::string>> tracker_names;
        for (const auto& [tracker, name] : trackers)
        {
            if (!tracker)
            {
                throw std::invalid_argument("RemoteSessionHost.start_session: tracker '" + name + "' is None");
            }
            tracker_list.push_back(tracker);
            tracker_names.emplace_back(tracker.get(), name);
        }
        {
            // A close() on another thread holds the mutex while it lingers.
            py::gil_scoped_release release;
            std::lock_guard<std::mutex> lock(mutex_);
            check_open();
            host_->start_session(tracker_list, tracker_names, handles, vendor_config, topics);
        }
        trackers_ = std::move(tracker_list);
    }

    void tick()
    {
        py::gil_scoped_release release;
        std::lock_guard<std::mutex> lock(mutex_);
        check_open();
        host_->tick();
    }

    bool alive() const
    {
        return host_->alive();
    }

    std::string close_reason() const
    {
        return host_->close_reason();
    }

    void close()
    {
        {
            py::gil_scoped_release release;
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            host_->close();
        }
        trackers_.clear();
    }

    PyRemoteSessionHost& enter()
    {
        return *this;
    }

    void exit(py::object, py::object, py::object)
    {
        close();
    }

private:
    explicit PyRemoteSessionHost(std::unique_ptr<RemoteSessionHost> host) : host_(std::move(host))
    {
    }

    //! Call with mutex_ held.
    void check_open() const
    {
        if (closed_)
        {
            throw std::runtime_error("RemoteSessionHost has been closed");
        }
    }

    // The host, which ends the DeviceIOSession, goes before the trackers.
    std::vector<std::shared_ptr<ITracker>> trackers_;
    std::unique_ptr<RemoteSessionHost> host_;
    std::mutex mutex_;
    bool closed_ = false;
};

} // namespace core
