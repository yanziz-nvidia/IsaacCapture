// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <remote_link/link.hpp>
#include <remote_link/transport.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace core
{

class McapRecordStreamReader;

enum class RemoteReadyState
{
    Pending,
    Ready,
};

//! No HELLO_ACK arrived before the connect timeout. The Python bindings raise it as TimeoutError.
class RemoteConnectTimeoutError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/*!
 * @brief The robot end of a remote session: a Link to a Teleop Session Server that feeds a record stream.
 *
 * SETUP and FRAME messages go into `stream`, which a RemoteSession reads; FRAME capture times are mapped onto this
 * machine's clock. Every method is thread-safe. Once the server's ERROR text arrived, every failure reports it,
 * even after the link died for another reason.
 */
class RemoteSessionClient
{
public:
    /*!
     * @brief Connects to `endpoint` (see parse_remote()), sends `hello_json` as HELLO and waits for HELLO_ACK.
     * @param timeout Bounds connecting and the wait for HELLO_ACK together; negative waits without limit (the link
     *                still dies once the server goes quiet).
     * @param link_options Link timing; send_pings and name are set here.
     * @throws std::invalid_argument for a malformed endpoint; RemoteConnectTimeoutError when no HELLO_ACK arrives in
     *         time; std::runtime_error with the server's ERROR text, or when connecting fails or the link is lost.
     */
    static std::unique_ptr<RemoteSessionClient> connect(const std::string& endpoint,
                                                        std::string hello_json,
                                                        std::shared_ptr<McapRecordStreamReader> stream,
                                                        std::chrono::milliseconds timeout,
                                                        LinkOptions link_options = {});

    //! The same over a transport that is already connected.
    static std::unique_ptr<RemoteSessionClient> connect(std::unique_ptr<ITransport> transport,
                                                        std::string hello_json,
                                                        std::shared_ptr<McapRecordStreamReader> stream,
                                                        std::chrono::milliseconds timeout,
                                                        LinkOptions link_options = {});

    ~RemoteSessionClient();
    RemoteSessionClient(const RemoteSessionClient&) = delete;
    RemoteSessionClient& operator=(const RemoteSessionClient&) = delete;

    //! Ready once READY arrived; Pending after `timeout` (negative: no limit). Throws as throw_if_failed().
    RemoteReadyState wait_ready(std::chrono::milliseconds timeout);
    //! The newest STATUS or READY JSON.
    std::optional<std::string> latest_status() const;
    std::string hello_ack() const;
    //! Throws std::runtime_error with the server's ERROR text, or "Remote session lost: <reason>" once the link is
    //! down (close() included).
    void throw_if_failed() const;
    //! False once the link is down (close() included).
    bool alive() const;
    ClockEstimate clock() const;
    //! Idempotent; the stream receives nothing after it returns.
    void close();

private:
    struct State;

    RemoteSessionClient(std::unique_ptr<ITransport> transport,
                        std::shared_ptr<McapRecordStreamReader> stream,
                        LinkOptions link_options);

    void handle(ReceivedMessage&& message);
    void throw_if_failed(const std::string& lost_prefix) const;
    //! Waits for `done` (checked under the state mutex) until `timeout`; throws on failure.
    template <typename Done>
    bool wait_for(std::chrono::milliseconds timeout, const std::string& lost_prefix, Done done) const;

    std::unique_ptr<State> state_;
    // Declared last, so it is closed (its threads joined) before the state its handler writes goes away.
    std::unique_ptr<Link> link_;
};

} // namespace core
