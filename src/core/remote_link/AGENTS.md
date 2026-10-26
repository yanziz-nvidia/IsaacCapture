<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Agent notes — `remote_link`

**CRITICAL (non-optional):** Before editing this package, complete the mandatory **`AGENTS.md` preflight** in [`../../../AGENTS.md`](../../../AGENTS.md) and read [`../AGENTS.md`](../AGENTS.md).

## Scope

- Generic network layer of the remote teleop session: wire header, message types, TCP and WebSocket transports,
  `Listener`, `Link`, clock offset. Keep it **MCAP-, OpenXR- and DeviceIO-free**; the glue that knows about record
  streams and trackers lives in `deviceio_session`. `oxr_utils` is linked only for `os_time.hpp`.
- `messages.hpp` and `frame_header.hpp` are the single source of truth for the wire format. JSON payloads are opaque
  bytes here; C++ never parses them.

## Threading

- `Link` owns two threads: the receiver runs `wait_ready()` first, then `receive()`; the sender sends nothing until
  that handshake succeeded, even while closing (close() bounds that wait by `close_drain`). Only the sender calls
  `ITransport::send()` (the WebSocket transport also sends PONG/CLOSE frames from `receive()` under its own send
  mutex).
- A latest-wins payload never goes out ahead of a reliable message queued before it: a FRAME may depend on the
  SETUP queued just before it.
- When the last message matters (ERROR, a refusal), end with `close_gracefully()` / a write half-close and read until
  the peer closes. Closing a TCP socket with unread input resets the connection, which discards what is still in the
  send queue.
- `MessageHandler` runs on the receive thread: it must not block, must not take the GIL or call into Python, and must
  not call `Link::close()` (that joins the calling thread).
- A `MergeFn` runs on the `send_latest()` caller's thread under the Link mutex; keep it a linear pass.
- Link threads never log per message. Death is logged once at warn; nothing reconnects.

## Windows

- The library must compile on Windows. Every POSIX facility stays in `tcp_transport.cpp` behind `#ifndef _WIN32`,
  with Windows bodies that throw `"Remote sessions require POSIX sockets; not supported on Windows"`. The WebSocket
  transport and `Link` are written against `TcpSocket` / `ITransport` only, so they need no guards. Public headers
  include no POSIX headers.
- Socket tests are gated `if(NOT WIN32)` in `tests/cpp/core/remote_link/CMakeLists.txt`.

## Testing

- Test `Link` with an in-memory transport whose `send()` can be held (blocks like a full socket) and whose
  `wait_writable()` reports the hold. Never rely on filling kernel socket buffers: their sizes differ per machine and
  the tests become flaky.
- Use `socketpair()` or loopback on port 0 for transport tests; never a fixed port.
- On loopback, data already delivered survives a reset; a graceful-close test needs data still queued at the closer,
  so it gives the peer an explicitly small `SO_RCVBUF` (set before connect) and stays under `TCP_NOTSENT_LOWAT`, which
  also bounds a blocking send.
