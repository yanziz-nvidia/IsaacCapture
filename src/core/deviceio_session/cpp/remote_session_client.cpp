// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "inc/deviceio_session/remote_session_client.hpp"

#include <log_bridge/logger.hpp>
#include <mcap/record_stream.hpp>
#include <remote_link/messages.hpp>
#include <remote_link/websocket_transport.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace core
{

namespace
{

// Link death notifies nobody, so waits wake this often to check it.
constexpr std::chrono::milliseconds kLivenessPoll{ 20 };

constexpr const char* kLostPrefix = "Remote session lost: ";

std::string to_text(const std::vector<uint8_t>& payload)
{
    return std::string(payload.begin(), payload.end());
}

} // namespace

struct RemoteSessionClient::State
{
    std::shared_ptr<McapRecordStreamReader> stream;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::string> hello_ack;
    std::optional<std::string> status;
    std::optional<std::string> error;
    bool ready = false;
};

RemoteSessionClient::RemoteSessionClient(std::unique_ptr<ITransport> transport,
                                         std::shared_ptr<McapRecordStreamReader> stream,
                                         LinkOptions link_options)
    : state_(std::make_unique<State>())
{
    if (!stream)
    {
        throw std::invalid_argument("RemoteSessionClient: no stream");
    }
    state_->stream = std::move(stream);
    link_options.send_pings = true;
    link_options.name = "RemoteSessionClient";
    link_ = std::make_unique<Link>(std::move(transport), std::move(link_options),
                                   [this](ReceivedMessage&& message) { handle(std::move(message)); });
}

RemoteSessionClient::~RemoteSessionClient()
{
    close();
}

std::unique_ptr<RemoteSessionClient> RemoteSessionClient::connect(const std::string& endpoint,
                                                                  std::string hello_json,
                                                                  std::shared_ptr<McapRecordStreamReader> stream,
                                                                  std::chrono::milliseconds timeout,
                                                                  LinkOptions link_options)
{
    if (!stream)
    {
        throw std::invalid_argument("RemoteSessionClient: no stream");
    }
    const RemoteAddress remote = parse_remote(endpoint);
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<ITransport> transport = connect_transport(remote, timeout);
    if (timeout.count() >= 0)
    {
        const auto spent =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        timeout = std::max(std::chrono::milliseconds{ 0 }, timeout - spent);
    }
    auto client =
        connect(std::move(transport), std::move(hello_json), std::move(stream), timeout, std::move(link_options));
    isaaccapture::Logger::get("isaaccapture.core.RemoteSessionClient")
        ->info("RemoteSessionClient: connected to {}: {}", endpoint, client->hello_ack());
    return client;
}

std::unique_ptr<RemoteSessionClient> RemoteSessionClient::connect(std::unique_ptr<ITransport> transport,
                                                                  std::string hello_json,
                                                                  std::shared_ptr<McapRecordStreamReader> stream,
                                                                  std::chrono::milliseconds timeout,
                                                                  LinkOptions link_options)
{
    std::unique_ptr<RemoteSessionClient> client(
        new RemoteSessionClient(std::move(transport), std::move(stream), std::move(link_options)));
    client->link_->send_reliable(MessageType::Hello, std::vector<uint8_t>(hello_json.begin(), hello_json.end()));
    client->link_->start();
    if (!client->wait_for(timeout, "Remote session ended before HELLO_ACK: ",
                          [&client] { return client->state_->hello_ack.has_value(); }))
    {
        throw RemoteConnectTimeoutError("No HELLO_ACK from the Teleop Session Server before the connect timeout");
    }
    return client;
}

void RemoteSessionClient::handle(ReceivedMessage&& message)
{
    switch (message.type)
    {
    case MessageType::Setup:
        state_->stream->add_setup(message.payload);
        return;
    case MessageType::Frame:
    {
        const FramePrefix prefix = decode_frame_prefix(message.payload);
        const ClockEstimate clock = link_->clock();
        // Every received header adds a clock sample before the handler runs, so this does not happen.
        if (!clock.valid)
        {
            return;
        }
        state_->stream->add_frame(std::span<const uint8_t>(message.payload).subspan(kFramePrefixSize), prefix.frame_seq,
                                  prefix.capture_time_ns + clock.offset_ns);
        return;
    }
    case MessageType::HelloAck:
    case MessageType::Status:
    case MessageType::Ready:
    case MessageType::Error:
        break;
    default:
        return; // messages of later protocol versions
    }

    std::string text = to_text(message.payload);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        switch (message.type)
        {
        case MessageType::HelloAck:
            state_->hello_ack = std::move(text);
            break;
        case MessageType::Ready:
            state_->ready = true;
            state_->status = std::move(text);
            break;
        case MessageType::Status:
            state_->status = std::move(text);
            break;
        default: // Error: the first one is the cause
            if (!state_->error)
            {
                state_->error = text.empty() ? "The Teleop Session Server ended the session" : std::move(text);
            }
            break;
        }
    }
    state_->cv.notify_all();
}

void RemoteSessionClient::throw_if_failed(const std::string& lost_prefix) const
{
    // Read before the error: by the time the link is down, an ERROR received before that is stored.
    const bool alive = link_->alive();
    const std::string reason = alive ? std::string() : link_->close_reason();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->error)
    {
        throw std::runtime_error(*state_->error);
    }
    if (!alive)
    {
        throw std::runtime_error(lost_prefix + reason);
    }
}

void RemoteSessionClient::throw_if_failed() const
{
    throw_if_failed(kLostPrefix);
}

template <typename Done>
bool RemoteSessionClient::wait_for(std::chrono::milliseconds timeout, const std::string& lost_prefix, Done done) const
{
    const bool bounded = timeout.count() >= 0;
    const auto deadline = std::chrono::steady_clock::now() + (bounded ? timeout : std::chrono::milliseconds{ 0 });
    while (true)
    {
        throw_if_failed(lost_prefix);
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (done())
        {
            return true;
        }
        auto wait = kLivenessPoll;
        if (bounded)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return false;
            }
            wait = std::min(wait, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        }
        state_->cv.wait_for(lock, wait);
    }
}

RemoteReadyState RemoteSessionClient::wait_ready(std::chrono::milliseconds timeout)
{
    return wait_for(timeout, kLostPrefix, [this] { return state_->ready; }) ? RemoteReadyState::Ready :
                                                                              RemoteReadyState::Pending;
}

std::optional<std::string> RemoteSessionClient::latest_status() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}

std::string RemoteSessionClient::hello_ack() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->hello_ack.value_or(std::string());
}

bool RemoteSessionClient::alive() const
{
    return link_->alive();
}

ClockEstimate RemoteSessionClient::clock() const
{
    return link_->clock();
}

void RemoteSessionClient::close()
{
    link_->close();
}

} // namespace core
