// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deviceio_base/tracker.hpp>
#include <log_bridge/logger.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core
{

class McapRecordStreamReader;

/**
 * @brief Configuration for RemoteSession.
 *
 * tracker_names maps each ITracker pointer to the base name its records carry on the stream;
 * the session recording them on the other end must use the same names.
 */
struct RemoteSessionConfig
{
    std::shared_ptr<McapRecordStreamReader> stream;
    std::vector<std::pair<const ITracker*, std::string>> tracker_names;
    //! The newest frame must be at most this old for update() to publish it.
    int64_t max_frame_age_ns = 250'000'000;
};

// Remote session -- reads tracker data from a live record stream that a session on another
// machine records. Whatever receives the stream feeds `stream`; this session only reads it.
class RemoteSession : public ITrackerSession
{
public:
    /**
     * @brief Creates an impl per tracker, each subscribing to its topics on the stream.
     * @throws std::invalid_argument for a missing stream, a null tracker, or a MessageChannelTracker,
     *         whose per-message records a stream that keeps only the newest record cannot carry.
     */
    static std::unique_ptr<RemoteSession> run(RemoteSessionConfig config);

    /**
     * @brief Publishes the stream's newest frame to every tracker. Never waits for data.
     *
     * When the newest frame is older than max_frame_age_ns, every tracker reports no data
     * until a fresh frame arrives.
     */
    void update();

    //! Whether the last update() published a fresh frame.
    bool is_fresh() const
    {
        return fresh_;
    }

    //! Age of the frame the last update() published, or nullopt when it published none. Slightly
    //! negative when the clock offset estimate is off by more than the transit time.
    std::optional<int64_t> frame_age_ns() const
    {
        return frame_age_ns_;
    }

    const ITrackerImpl& get_tracker_impl(const ITracker& tracker) const override
    {
        auto it = tracker_impls_.find(&tracker);
        if (it == tracker_impls_.end())
        {
            throw std::runtime_error("Tracker implementation not found for tracker: " + std::string(tracker.get_name()));
        }
        return *(it->second);
    }

private:
    explicit RemoteSession(RemoteSessionConfig config);

    std::shared_ptr<spdlog::logger> logger_ = isaaccapture::Logger::get("isaaccapture.core.RemoteSession");
    // Declared before the impls, which read it, so it is destroyed after them.
    std::shared_ptr<McapRecordStreamReader> stream_;
    int64_t max_frame_age_ns_;
    bool fresh_ = false;
    std::optional<int64_t> frame_age_ns_;
    std::unordered_map<const ITracker*, std::unique_ptr<ITrackerImpl>> tracker_impls_;
};

} // namespace core
