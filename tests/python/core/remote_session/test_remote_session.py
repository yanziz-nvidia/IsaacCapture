# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""A RemoteSessionHost and a RemoteSession over loopback, in one process.

The server runs on a thread. Its KeyboardTracker never touches the OpenXR session handles, so
placeholder handles stand in for a runtime.
"""

import json
import os
import socket
import sys
import threading
import time

import pytest

from isaaccapture import oxr
from isaaccapture.deviceio_session import (
    RemoteSession,
    RemoteSessionHost,
    RemoteSessionListener,
    VendorConfig,
)
from isaaccapture.deviceio_trackers import EvdevKeyCode, KeyboardTracker

pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="remote sessions need POSIX sockets"
)

WAIT_MS = 5000
KEY_W = int(EvdevKeyCode.KeyW)
HELLO_ACK = {"session_id": "test", "server_version": "0"}
READY = {"stage": "ready", "message": ""}
TRANSPORTS = pytest.mark.parametrize("websocket", [False, True], ids=["tcp", "ws"])


def _endpoint(listener, websocket):
    return f"{'ws' if websocket else 'tcp'}://127.0.0.1:{listener.port}"


def _wait_for(predicate, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    while not predicate():
        if time.monotonic() > deadline:
            return False
        time.sleep(0.002)
    return True


class _Server:
    """Accepts one robot and streams a keyboard that holds W, on its own thread.

    Sends READY at once unless `ready` is False; on stop() sends `final_error` (if any) and closes.
    """

    def __init__(self, websocket=False, ready=True, final_error=None):
        self.listener = RemoteSessionListener("127.0.0.1", 0)
        self.websocket = websocket
        self.endpoint = _endpoint(self.listener, websocket)
        self.hello = None
        self.error = None
        self._ready = ready
        self._final_error = final_error
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        try:
            accepted = self.listener.accept(WAIT_MS)
            assert accepted is not None, "no robot connected"
            with RemoteSessionHost.adopt(accepted[0], self.websocket) as host:
                self.hello = json.loads(host.wait_hello(WAIT_MS))
                host.send_hello_ack(json.dumps(HELLO_ACK))
                keyboard = KeyboardTracker()
                host.start_session(
                    [(keyboard, "0")],
                    oxr.OpenXRSessionHandles(1, 1, 1, 1),
                    VendorConfig(),
                    self.hello["topics"],
                )
                provider = keyboard.create_provider()
                provider.key_down(KEY_W)
                if self._ready:
                    host.send_ready(json.dumps(READY))
                while not self._stop.is_set() and host.alive:
                    host.tick()
                    time.sleep(0.005)
                if self._final_error is not None:
                    host.send_error(self._final_error)
        except Exception as e:  # reported by stop()
            self.error = e

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=10)
        assert not self._thread.is_alive()
        if self.error is not None:
            raise self.error


def _robot():
    keyboard = KeyboardTracker()
    return keyboard, RemoteSession.create([(keyboard, "0")])


def _hello(session):
    return json.dumps({"protocol": 1, "topics": session.subscribed_topics()})


def _holds_w(keyboard, session):
    session.update()
    data = keyboard.get_keyboard_data(session)
    return data is not None and list(data.held_keys) == [KEY_W]


@TRANSPORTS
def test_robot_reads_the_server_keyboard(websocket):
    keyboard, session = _robot()
    server = _Server(websocket)
    with session:
        session.connect(server.endpoint, _hello(session), WAIT_MS)
        assert json.loads(session.hello_ack()) == HELLO_ACK
        assert session.wait_ready(WAIT_MS)
        assert json.loads(session.latest_status()) == READY

        assert _wait_for(lambda: _holds_w(keyboard, session))
        assert session.is_fresh
        assert session.frame_age_ns is not None
        assert abs(session.frame_age_ns) < 250_000_000
    server.stop()
    assert server.hello["topics"] == ["0/keyboard"]
    with pytest.raises(RuntimeError, match="closed"):
        session.update()


def test_wait_ready_is_false_until_ready():
    keyboard, session = _robot()
    server = _Server(ready=False)
    with session:
        session.connect(server.endpoint, _hello(session), WAIT_MS)
        start = time.monotonic()
        assert session.wait_ready(100) is False
        assert time.monotonic() - start >= 0.1
        assert session.latest_status() is None
        # Frames flow before READY.
        assert _wait_for(lambda: _holds_w(keyboard, session))
    server.stop()


def _update_error(session):
    try:
        session.update()
    except RuntimeError as e:
        return str(e)
    return None


def test_update_raises_once_the_server_closes():
    keyboard, session = _robot()
    server = _Server()
    with session:
        session.connect(server.endpoint, _hello(session), WAIT_MS)
        assert session.wait_ready(WAIT_MS)
        assert _wait_for(lambda: _holds_w(keyboard, session))
        server.stop()
        assert _wait_for(lambda: _update_error(session) is not None)
        assert _update_error(session).startswith("Remote session lost: ")
        with pytest.raises(RuntimeError, match="Remote session lost"):
            session.wait_ready(0)


@TRANSPORTS
def test_update_raises_the_server_error_text(websocket):
    keyboard, session = _robot()
    server = _Server(websocket, final_error="headset lost")
    with session:
        session.connect(server.endpoint, _hello(session), WAIT_MS)
        assert _wait_for(lambda: _holds_w(keyboard, session))
        server.stop()
        assert _wait_for(lambda: _update_error(session) is not None)
        assert _update_error(session) == "headset lost"


@TRANSPORTS
def test_connect_raises_the_text_of_a_refusal(websocket):
    listener = RemoteSessionListener("127.0.0.1", 0)
    busy = "busy: another robot app is connected to this Teleop Session Server"

    def refuse():
        accepted = listener.accept(WAIT_MS)
        if accepted is not None:
            RemoteSessionListener.reject(accepted[0], websocket, busy)

    refuser = threading.Thread(target=refuse)
    refuser.start()
    _, session = _robot()
    with session:
        with pytest.raises(RuntimeError) as raised:
            session.connect(_endpoint(listener, websocket), _hello(session), WAIT_MS)
    refuser.join()
    assert str(raised.value) == busy


def test_connect_times_out_without_hello_ack():
    listener = RemoteSessionListener("127.0.0.1", 0)
    accepted = []
    acceptor = threading.Thread(
        target=lambda: accepted.append(listener.accept(WAIT_MS))
    )
    acceptor.start()
    _, session = _robot()
    try:
        with session:
            with pytest.raises(TimeoutError, match="HELLO_ACK"):
                session.connect(_endpoint(listener, False), _hello(session), 300)
    finally:
        acceptor.join()
        if accepted and accepted[0] is not None:
            os.close(accepted[0][0])


def test_close_ends_a_wait_ready_on_another_thread():
    _, session = _robot()
    server = _Server(ready=False)
    session.connect(server.endpoint, _hello(session), WAIT_MS)
    outcome = []

    def wait():
        try:
            outcome.append(session.wait_ready(WAIT_MS))
        except RuntimeError as e:
            outcome.append(e)

    waiter = threading.Thread(target=wait)
    waiter.start()
    time.sleep(0.1)
    start = time.monotonic()
    session.close()
    waiter.join()
    assert time.monotonic() - start < 2.0
    assert isinstance(outcome[0], RuntimeError)
    server.stop()


def test_host_refuses_work_after_close():
    listener = RemoteSessionListener("127.0.0.1", 0)
    with socket.create_connection(("127.0.0.1", listener.port)) as robot:
        fd, peer = listener.accept(WAIT_MS)
        assert peer == f"127.0.0.1:{robot.getsockname()[1]}"
        host = RemoteSessionHost.adopt(fd, False)
        with pytest.raises(RuntimeError, match="start_session"):
            host.tick()
        assert host.alive
        assert host.close_reason == ""
        robot.close()  # lets close() skip its linger
        host.close()
        assert not host.alive
        assert host.close_reason
        with pytest.raises(RuntimeError, match="closed"):
            host.tick()
        with pytest.raises(RuntimeError, match="closed"):
            host.start_session(
                [(KeyboardTracker(), "0")],
                oxr.OpenXRSessionHandles(1, 1, 1, 1),
                VendorConfig(),
                [],
            )
        host.close()  # idempotent


def _runs_alongside(call):
    """True when this thread keeps running while `call` blocks for about 1.5 s on another thread."""

    def target():
        try:
            call()
        except (RuntimeError, TimeoutError):
            pass

    thread = threading.Thread(target=target)
    # Each step here needs the GIL (start() included), so a call that holds it shows up as one long gap.
    longest = 0.0
    last = time.monotonic()
    thread.start()
    while thread.is_alive():
        time.sleep(0.01)
        now = time.monotonic()
        longest = max(longest, now - last)
        last = now
    thread.join()
    return longest < 0.75


def test_blocking_calls_release_the_gil():
    listener = RemoteSessionListener("127.0.0.1", 0)
    assert _runs_alongside(lambda: listener.accept(1500))

    # A robot that never sends HELLO.
    with socket.create_connection(("127.0.0.1", listener.port)):
        fd, _ = listener.accept(WAIT_MS)
        with RemoteSessionHost.adopt(fd, False) as host:
            assert _runs_alongside(lambda: host.wait_hello(1500))

    # A server that never answers HELLO.
    accepted = []
    acceptor = threading.Thread(
        target=lambda: accepted.append(listener.accept(WAIT_MS))
    )
    acceptor.start()
    _, session = _robot()
    with session:
        assert _runs_alongside(
            lambda: session.connect(_endpoint(listener, False), "{}", 1500)
        )
    acceptor.join()
    os.close(accepted[0][0])

    _, session = _robot()
    server = _Server(ready=False)
    with session:
        session.connect(server.endpoint, _hello(session), WAIT_MS)
        assert _runs_alongside(lambda: session.wait_ready(1500))
    server.stop()


def test_dropping_an_open_host_releases_the_gil():
    listener = RemoteSessionListener("127.0.0.1", 0)
    # The robot stays connected, so the host's teardown lingers for it to close.
    with socket.create_connection(("127.0.0.1", listener.port)):
        fd, _ = listener.accept(WAIT_MS)
        hosts = [RemoteSessionHost.adopt(fd, False)]
        start = time.monotonic()
        assert _runs_alongside(hosts.clear)
        assert time.monotonic() - start >= 0.5


def test_start_session_waits_for_a_closing_host_without_the_gil():
    listener = RemoteSessionListener("127.0.0.1", 0)
    with socket.create_connection(("127.0.0.1", listener.port)) as robot:
        fd, _ = listener.accept(WAIT_MS)
        host = RemoteSessionHost.adopt(fd, False)
        closer = threading.Thread(target=host.close)
        closer.start()
        # End of stream: close() has half-closed and lingers for the robot, holding the host.
        robot.settimeout(WAIT_MS / 1000)
        while robot.recv(4096):
            pass
        assert _runs_alongside(
            lambda: host.start_session(
                [(KeyboardTracker(), "0")],
                oxr.OpenXRSessionHandles(1, 1, 1, 1),
                VendorConfig(),
                [],
            )
        )
        closer.join()


def test_dropping_an_open_robot_session_disconnects_it():
    _, session = _robot()
    server = _Server()
    session.connect(server.endpoint, _hello(session), WAIT_MS)
    assert session.wait_ready(WAIT_MS)
    del session
    # The server's loop ends once its link is down.
    assert _wait_for(lambda: not server._thread.is_alive())
    server.stop()
