// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "inc/remote_link/websocket_transport.hpp"

#include "inc/remote_link/frame_header.hpp"
#include "ws_handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <stdexcept>
#include <utility>

namespace core
{

namespace
{

using remote_link_detail::base64;
using remote_link_detail::websocket_accept_key;

constexpr std::chrono::milliseconds kStall{ 3000 }; // matches TcpTransport's mid-message stall
constexpr size_t kMaxHttpHead = 8192;
constexpr uint64_t kMaxMessage = kHeaderSize + kMaxPayloadSize;

constexpr uint8_t kContinuation = 0x0;
constexpr uint8_t kBinary = 0x2;
constexpr uint8_t kClose = 0x8;
constexpr uint8_t kPing = 0x9;
constexpr uint8_t kPong = 0xA;

std::string lower(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct HttpHead
{
    std::string start_line;
    std::map<std::string, std::string> headers; //!< lowercase names, trimmed values
};

//! Reads up to the blank line one byte at a time so no WebSocket bytes that follow are consumed.
ReceiveResult read_http_head(TcpSocket& socket, std::chrono::milliseconds timeout, HttpHead& head)
{
    std::string text;
    while (text.size() < 4 || text.compare(text.size() - 4, 4, "\r\n\r\n") != 0)
    {
        if (text.size() >= kMaxHttpHead)
        {
            return ReceiveResult::Malformed;
        }
        uint8_t byte = 0;
        const TcpSocket::RecvResult result = socket.recv_all(&byte, 1, text.empty() ? timeout : kStall);
        if (result != TcpSocket::RecvResult::Ok)
        {
            return result == TcpSocket::RecvResult::Timeout && text.empty() ? ReceiveResult::Timeout :
                                                                              ReceiveResult::Closed;
        }
        text.push_back(static_cast<char>(byte));
    }
    size_t begin = text.find("\r\n");
    head.start_line = text.substr(0, begin);
    for (begin += 2; begin < text.size() - 2;)
    {
        const size_t end = text.find("\r\n", begin);
        const std::string line = text.substr(begin, end - begin);
        begin = end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            return ReceiveResult::Malformed;
        }
        const size_t value = line.find_first_not_of(" \t", colon + 1);
        head.headers[lower(line.substr(0, colon))] =
            value == std::string::npos ? "" : line.substr(value, line.find_last_not_of(" \t") + 1 - value);
    }
    return ReceiveResult::Ok;
}

std::string header_value(const HttpHead& head, const std::string& name)
{
    const auto it = head.headers.find(name);
    return it == head.headers.end() ? "" : it->second;
}

bool send_text(TcpSocket& socket, const std::string& text)
{
    return socket.send_all(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool starts_with(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

} // namespace

RemoteAddress parse_remote(const std::string& text)
{
    if (starts_with(text, "wss://"))
    {
        throw std::invalid_argument("wss:// is not supported yet; terminate TLS in front of the server");
    }
    RemoteAddress remote;
    std::string authority = text;
    uint16_t default_port = kDefaultTcpPort;
    if (starts_with(text, "ws://"))
    {
        remote.websocket = true;
        default_port = kDefaultWebSocketPort;
        authority = text.substr(5);
        const size_t slash = authority.find('/');
        if (slash != std::string::npos)
        {
            remote.path = authority.substr(slash);
            authority.resize(slash);
        }
    }
    else if (starts_with(text, "tcp://"))
    {
        authority = text.substr(6);
    }
    else if (text.find("://") != std::string::npos)
    {
        throw std::invalid_argument("unsupported scheme in '" + text + "'; use tcp:// or ws://");
    }
    if (!remote.websocket && authority.find('/') != std::string::npos)
    {
        throw std::invalid_argument("a TCP endpoint takes no path: '" + text + "'");
    }
    remote.endpoint = parse_endpoint(authority, default_port);
    return remote;
}

std::unique_ptr<ITransport> connect_transport(const RemoteAddress& remote, std::chrono::milliseconds timeout)
{
    const bool bounded = timeout.count() >= 0;
    const auto deadline = std::chrono::steady_clock::now() + (bounded ? timeout : std::chrono::milliseconds{ 0 });
    TcpSocket socket = TcpSocket::connect(remote.endpoint, timeout);
    if (!remote.websocket)
    {
        return std::make_unique<TcpTransport>(std::move(socket));
    }
    const auto remaining =
        bounded ? std::max(std::chrono::milliseconds{ 1 }, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               deadline - std::chrono::steady_clock::now())) :
                  timeout;
    const bool v6 = remote.endpoint.host.find(':') != std::string::npos;
    const std::string host =
        (v6 ? "[" + remote.endpoint.host + "]" : remote.endpoint.host) + ":" + std::to_string(remote.endpoint.port);
    return std::make_unique<WebSocketTransport>(
        std::move(socket), WebSocketTransport::Role::Client, host, remote.path, remaining);
}

std::unique_ptr<ITransport> adopt_transport(int fd, bool websocket)
{
    TcpSocket socket(fd);
    if (!websocket)
    {
        return std::make_unique<TcpTransport>(std::move(socket));
    }
    return std::make_unique<WebSocketTransport>(std::move(socket), WebSocketTransport::Role::Server);
}

WebSocketTransport::WebSocketTransport(
    TcpSocket socket, Role role, const std::string& host, const std::string& path, std::chrono::milliseconds timeout)
    : m_socket(std::move(socket)), m_role(role), m_mask_rng(std::random_device{}())
{
    if (role == Role::Server)
    {
        return;
    }
    std::array<uint8_t, 16> nonce{};
    std::generate(nonce.begin(), nonce.end(), [this] { return static_cast<uint8_t>(m_mask_rng()); });
    const std::string key = base64(nonce.data(), nonce.size());
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    const std::string where = "WebSocket upgrade to " + host + path;
    HttpHead response;
    if (!send_text(m_socket, request))
    {
        throw std::runtime_error(where + ": send failed");
    }
    if (read_http_head(m_socket, timeout, response) != ReceiveResult::Ok)
    {
        throw std::runtime_error(where + ": no valid HTTP response");
    }
    if (!starts_with(response.start_line, "HTTP/1.1 101"))
    {
        throw std::runtime_error(where + " refused: " + response.start_line);
    }
    if (header_value(response, "sec-websocket-accept") != websocket_accept_key(key))
    {
        throw std::runtime_error(where + ": wrong Sec-WebSocket-Accept");
    }
    m_open = true;
}

ReceiveResult WebSocketTransport::accept_upgrade(std::chrono::milliseconds timeout)
{
    HttpHead request;
    const ReceiveResult result = read_http_head(m_socket, timeout, request);
    if (result == ReceiveResult::Timeout || result == ReceiveResult::Closed)
    {
        return result;
    }
    const std::string key = header_value(request, "sec-websocket-key");
    if (result != ReceiveResult::Ok || !starts_with(request.start_line, "GET ") ||
        lower(header_value(request, "upgrade")).find("websocket") == std::string::npos ||
        lower(header_value(request, "connection")).find("upgrade") == std::string::npos ||
        header_value(request, "sec-websocket-version") != "13" || key.empty())
    {
        send_text(m_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        m_socket.shutdown();
        return ReceiveResult::Malformed;
    }
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
        websocket_accept_key(key) + "\r\n\r\n";
    if (!send_text(m_socket, response))
    {
        return ReceiveResult::Closed;
    }
    m_open = true;
    return ReceiveResult::Ok;
}

ReceiveResult WebSocketTransport::wait_ready(std::chrono::milliseconds timeout)
{
    return m_open.load() ? ReceiveResult::Ok : accept_upgrade(timeout);
}

bool WebSocketTransport::send(const uint8_t* message, size_t size)
{
    return m_open.load() && send_frame(kBinary, message, size);
}

bool WebSocketTransport::wait_writable(std::chrono::milliseconds timeout)
{
    return m_open.load() && m_socket.wait_writable(timeout);
}

void WebSocketTransport::shutdown()
{
    m_socket.shutdown();
}

void WebSocketTransport::shutdown_write()
{
    std::lock_guard<std::mutex> lock(m_send_mutex); // not in the middle of a PONG from receive()
    m_socket.shutdown_write();
}

bool WebSocketTransport::send_frame(uint8_t opcode, const uint8_t* data, size_t size)
{
    std::lock_guard<std::mutex> lock(m_send_mutex);
    std::vector<uint8_t>& out = m_send_buffer;
    out.assign(1, static_cast<uint8_t>(0x80 | opcode)); // FIN: never fragmented on send
    const uint8_t mask_bit = m_role == Role::Client ? 0x80 : 0x00;
    if (size < 126)
    {
        out.push_back(static_cast<uint8_t>(mask_bit | size));
    }
    else
    {
        const int length_bytes = size <= 0xFFFF ? 2 : 8;
        out.push_back(static_cast<uint8_t>(mask_bit | (length_bytes == 2 ? 126 : 127)));
        for (int i = length_bytes - 1; i >= 0; --i)
        {
            out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (8 * i)));
        }
    }
    std::array<uint8_t, 4> mask{};
    if (mask_bit != 0)
    {
        const uint32_t key = static_cast<uint32_t>(m_mask_rng());
        mask = { static_cast<uint8_t>(key >> 24), static_cast<uint8_t>(key >> 16), static_cast<uint8_t>(key >> 8),
                 static_cast<uint8_t>(key) };
        out.insert(out.end(), mask.begin(), mask.end());
    }
    const size_t payload_at = out.size();
    out.insert(out.end(), data, data + size);
    if (mask_bit != 0)
    {
        for (size_t i = 0; i < size; ++i)
        {
            out[payload_at + i] ^= mask[i % 4];
        }
    }
    return m_socket.send_all(out.data(), out.size());
}

ReceiveResult WebSocketTransport::receive(std::vector<uint8_t>& message, std::chrono::milliseconds first_byte_timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + first_byte_timeout;
    if (!m_open.load())
    {
        const ReceiveResult upgraded = accept_upgrade(first_byte_timeout);
        if (upgraded != ReceiveResult::Ok)
        {
            return upgraded;
        }
    }
    message.clear();
    bool in_message = false;
    while (true)
    {
        // The caller's timeout covers the wait for a message to start (control frames included); inside one, a
        // stall is a drop.
        const auto remaining =
            std::max(std::chrono::milliseconds{ 0 },
                     std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        uint8_t head[2];
        const TcpSocket::RecvResult first = m_socket.recv_all(head, sizeof(head), in_message ? kStall : remaining);
        if (first != TcpSocket::RecvResult::Ok)
        {
            return first == TcpSocket::RecvResult::Timeout && !in_message ? ReceiveResult::Timeout :
                                                                            ReceiveResult::Closed;
        }
        const bool fin = (head[0] & 0x80) != 0;
        const uint8_t opcode = head[0] & 0x0F;
        const bool masked = (head[1] & 0x80) != 0;
        const bool control = (opcode & 0x08) != 0;
        uint64_t length = head[1] & 0x7F;
        // RSV bits need an extension we never negotiate; clients must mask and servers must not.
        if ((head[0] & 0x70) != 0 || masked != (m_role == Role::Server))
        {
            return ReceiveResult::Malformed;
        }
        if (length >= 126)
        {
            std::array<uint8_t, 8> ext{};
            const size_t ext_size = length == 126 ? 2 : 8;
            if (m_socket.recv_all(ext.data(), ext_size, kStall) != TcpSocket::RecvResult::Ok)
            {
                return ReceiveResult::Closed;
            }
            length = 0;
            for (size_t i = 0; i < ext_size; ++i)
            {
                length = (length << 8) | ext[i];
            }
        }
        std::array<uint8_t, 4> mask{};
        if (masked && m_socket.recv_all(mask.data(), mask.size(), kStall) != TcpSocket::RecvResult::Ok)
        {
            return ReceiveResult::Closed;
        }

        std::vector<uint8_t> control_payload;
        uint8_t* dest = nullptr;
        if (control)
        {
            if (!fin || length > 125)
            {
                return ReceiveResult::Malformed;
            }
            control_payload.resize(static_cast<size_t>(length));
            dest = control_payload.data();
        }
        else
        {
            // Text frames, unknown opcodes and broken fragmentation.
            if (opcode != (in_message ? kContinuation : kBinary))
            {
                return ReceiveResult::Malformed;
            }
            if (length > kMaxMessage - message.size())
            {
                return ReceiveResult::Malformed;
            }
            const size_t offset = message.size();
            message.resize(offset + static_cast<size_t>(length));
            dest = message.data() + offset;
        }
        if (length > 0 && m_socket.recv_all(dest, static_cast<size_t>(length), kStall) != TcpSocket::RecvResult::Ok)
        {
            return ReceiveResult::Closed;
        }
        for (size_t i = 0; masked && i < length; ++i)
        {
            dest[i] ^= mask[i % 4];
        }

        if (opcode == kPing)
        {
            send_frame(kPong, control_payload.data(), control_payload.size());
            continue;
        }
        if (opcode == kPong)
        {
            continue;
        }
        if (opcode == kClose)
        {
            // Echo the status code, then report the peer gone.
            send_frame(kClose, control_payload.data(), std::min<size_t>(control_payload.size(), 2));
            return ReceiveResult::Closed;
        }
        if (control)
        {
            return ReceiveResult::Malformed; // reserved control opcodes
        }
        in_message = !fin;
        if (in_message)
        {
            continue;
        }
        if (message.size() < kHeaderSize)
        {
            return ReceiveResult::Malformed;
        }
        try
        {
            return message.size() == kHeaderSize + decode_header(message.data()).payload_size ? ReceiveResult::Ok :
                                                                                                ReceiveResult::Malformed;
        }
        catch (const FrameDecodeError&)
        {
            return ReceiveResult::Malformed;
        }
    }
}

} // namespace core
