// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "inc/deviceio_session/remote_session_host.hpp"

#include <log_bridge/logger.hpp>
#include <mcap/record_stream.hpp>
#include <oxr_utils/os_time.hpp>
#include <remote_link/messages.hpp>
#include <remote_link/websocket_transport.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <span>
#include <stdexcept>

namespace core
{

namespace
{

// Link death notifies nobody, so waits wake this often to check it.
constexpr std::chrono::milliseconds kLivenessPoll{ 20 };
// How long close() waits for the robot to close its end after the last message.
constexpr std::chrono::milliseconds kCloseLinger{ 1000 };

std::span<const uint8_t> frame_records(const std::vector<uint8_t>& payload)
{
    return std::span<const uint8_t>(payload).subspan(kFramePrefixSize);
}

} // namespace

std::vector<uint8_t> merge_frame_payloads(std::vector<uint8_t> older, std::vector<uint8_t> newer)
{
    decode_frame_prefix(older); // validates the length
    const FramePrefix prefix = decode_frame_prefix(newer);
    return encode_frame_payload(prefix, merge_frame_records(frame_records(older), frame_records(newer)));
}

struct RemoteSessionHost::State
{
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::string> hello;
};

RemoteSessionHost::RemoteSessionHost(std::unique_ptr<ITransport> transport, LinkOptions link_options)
    : state_(std::make_unique<State>())
{
    link_options.send_pings = false;
    link_options.name = "RemoteSessionHost";
    link_ = std::make_unique<Link>(std::move(transport), std::move(link_options),
                                   [this](ReceivedMessage&& message) { handle(std::move(message)); });
}

RemoteSessionHost::~RemoteSessionHost()
{
    close();
}

std::unique_ptr<RemoteSessionHost> RemoteSessionHost::adopt(int fd, bool websocket, LinkOptions link_options)
{
    return adopt(adopt_transport(fd, websocket), std::move(link_options));
}

std::unique_ptr<RemoteSessionHost> RemoteSessionHost::adopt(std::unique_ptr<ITransport> transport,
                                                            LinkOptions link_options)
{
    std::unique_ptr<RemoteSessionHost> host(new RemoteSessionHost(std::move(transport), std::move(link_options)));
    host->link_->start();
    return host;
}

void RemoteSessionHost::handle(ReceivedMessage&& message)
{
    if (message.type != MessageType::Hello)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->hello)
        {
            state_->hello = std::string(message.payload.begin(), message.payload.end());
        }
    }
    state_->cv.notify_all();
}

std::optional<std::string> RemoteSessionHost::wait_hello(std::chrono::milliseconds timeout)
{
    const bool bounded = timeout.count() >= 0;
    const auto deadline = std::chrono::steady_clock::now() + (bounded ? timeout : std::chrono::milliseconds{ 0 });
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (true)
    {
        if (!link_->alive())
        {
            throw std::runtime_error("Remote session lost: " + link_->close_reason());
        }
        if (state_->hello)
        {
            return state_->hello;
        }
        auto wait = kLivenessPoll;
        if (bounded)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return std::nullopt;
            }
            wait = std::min(wait, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        }
        state_->cv.wait_for(lock, wait);
    }
}

void RemoteSessionHost::send_text(MessageType type, const std::string& text)
{
    link_->send_reliable(type, std::vector<uint8_t>(text.begin(), text.end()));
}

void RemoteSessionHost::send_hello_ack(std::string json)
{
    send_text(MessageType::HelloAck, json);
}

void RemoteSessionHost::send_status(std::string json)
{
    send_text(MessageType::Status, json);
}

void RemoteSessionHost::send_ready(std::string json)
{
    send_text(MessageType::Ready, json);
}

void RemoteSessionHost::send_error(std::string text)
{
    send_text(MessageType::Error, text);
}

void RemoteSessionHost::start_session(const std::vector<std::shared_ptr<ITracker>>& trackers,
                                      const std::vector<std::pair<const ITracker*, std::string>>& tracker_names,
                                      const OpenXRSessionHandles& handles,
                                      const VendorConfig& vendor_config,
                                      const std::vector<std::string>& topics)
{
    if (session_)
    {
        throw std::logic_error("RemoteSessionHost: session already started");
    }
    // The filter applies from the first record, so it is set before the session's writer opens.
    auto writer = std::make_unique<McapRecordStreamWriter>();
    writer->set_topic_filter(topics);
    McapRecordingConfig recording;
    recording.tracker_names = tracker_names;
    recording.stream = writer.get();
    writer_ = std::move(writer);
    session_ = DeviceIOSession::run(trackers, handles, std::move(recording), vendor_config);
    isaaccapture::Logger::get("isaaccapture.core.RemoteSessionHost")
        ->info("RemoteSessionHost: streaming {} trackers, {} topics", tracker_names.size(), topics.size());

    // Channels are declared with their first record, so this is usually empty.
    std::vector<uint8_t> setup = writer_->take_setup();
    if (!setup.empty())
    {
        link_->send_reliable(MessageType::Setup, std::move(setup));
    }
}

void RemoteSessionHost::tick()
{
    if (!session_)
    {
        throw std::logic_error("RemoteSessionHost: tick() before start_session()");
    }
    const int64_t capture_ns = os_monotonic_now_ns();
    session_->update();
    // Reliable SETUP is queued first; the link never sends a FRAME ahead of it.
    std::vector<uint8_t> setup = writer_->take_setup();
    if (!setup.empty())
    {
        link_->send_reliable(MessageType::Setup, std::move(setup));
    }
    const std::vector<uint8_t> records = writer_->take_frame();
    link_->send_latest(MessageType::Frame, encode_frame_payload(FramePrefix{ next_frame_seq_++, capture_ns }, records),
                       merge_frame_payloads);
}

bool RemoteSessionHost::alive() const
{
    return link_->alive();
}

std::string RemoteSessionHost::close_reason() const
{
    return link_->close_reason();
}

void RemoteSessionHost::close()
{
    // The session writes the end of its recording into the writer, so it goes first.
    session_.reset();
    writer_.reset();
    link_->close_gracefully(kCloseLinger);
}

} // namespace core
