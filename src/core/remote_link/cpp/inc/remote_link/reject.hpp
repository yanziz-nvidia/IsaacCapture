// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace core
{

/*!
 * @brief Refuses an accepted connection with an ERROR message, then closes it. Blocks for up to about 3 s.
 *
 * Takes ownership of `fd`. For WebSocket the upgrade is completed first (up to 1 s). The peer's first message
 * (the HELLO) is read if it arrives within 1 s; then ERROR(`text`) is sent, the stream is half-closed and input is
 * discarded until the peer closes, for up to 1 s, so the peer reads the ERROR instead of a connection reset.
 */
void reject_remote_connection(int fd, bool websocket, const std::string& text);

} // namespace core
