"""
client.py — thin client for the hsed C daemon. The TUI never touches
/proc, ptrace, or ftruncate itself anymore; every privileged operation
goes through hsed's Unix domain socket protocol (see
backend/src/protocol.h for the wire format this mirrors).
"""
from __future__ import annotations

import base64
import json
import os
import socket
from dataclasses import dataclass
from typing import Iterator, Optional


class HsedError(RuntimeError):
    """A {"type": "error", ...} response from the daemon, or a transport
    failure (couldn't connect, connection dropped mid-response, etc)."""


def default_socket_path() -> str:
    override = os.environ.get("HSED_SOCKET")
    if override:
        return override
    if os.geteuid() == 0:
        return "/run/hsed.sock"
    return f"/tmp/hsed-{os.geteuid()}.sock"


@dataclass
class HiddenEntry:
    pid: int
    fd: int
    mode: str
    size: int
    uid: int
    user: str
    comm: str
    cmdline: str
    path: str
    inode: int
    dev_major: int
    dev_minor: int
    mtime: float

    @property
    def key(self) -> str:
        return f"{self.pid}:{self.fd}"

    @classmethod
    def from_json(cls, obj: dict) -> "HiddenEntry":
        return cls(
            pid=obj["pid"], fd=obj["fd"], mode=obj["mode"], size=obj["size"],
            uid=obj["uid"], user=obj["user"], comm=obj["comm"],
            cmdline=obj["cmdline"], path=obj["path"], inode=obj["inode"],
            dev_major=obj["dev_major"], dev_minor=obj["dev_minor"],
            mtime=obj["mtime"],
        )


@dataclass
class WriteEvent:
    tid: int
    ret: int
    captured: int
    is_writev: bool
    data: bytes

    @classmethod
    def from_json(cls, obj: dict) -> "WriteEvent":
        return cls(
            tid=obj["tid"], ret=obj["ret"], captured=obj["captured"],
            is_writev=obj["is_writev"], data=base64.b64decode(obj["data_b64"]),
        )


class _LineReader:
    """Small buffered line reader — recv'ing one byte at a time works but
    wastes a syscall per byte, which matters for STREAM on a busy fd."""

    def __init__(self, sock: socket.socket, bufsize: int = 4096) -> None:
        self._sock = sock
        self._bufsize = bufsize
        self._buf = b""

    def readline(self) -> Optional[str]:
        while b"\n" not in self._buf:
            chunk = self._sock.recv(self._bufsize)
            if not chunk:
                if self._buf:
                    line, self._buf = self._buf, b""
                    return line.decode(errors="replace")
                return None
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode(errors="replace")


def build_path_tree(entries: list[HiddenEntry]) -> dict:
    """
    Reconstructs a virtual directory tree from the original (now-deleted)
    paths, so the TUI can render a "hidden filesystem" view even though
    none of these paths exist on disk anymore. Leaves are lists of
    HiddenEntry (usually length 1; longer if several fds share a path).
    """
    root: dict = {}
    for e in entries:
        parts = [p for p in e.path.split("/") if p]
        node = root
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        if parts:
            bucket = node.setdefault(parts[-1], [])
            if isinstance(bucket, list):
                bucket.append(e)
    return root


class HsedClient:
    """
    One socket connection per call (or per STREAM session) — cheap on a
    local Unix socket, and it keeps the client free of any shared-state
    concurrency concerns in the TUI.
    """

    def __init__(self, socket_path: Optional[str] = None, timeout: float = 5.0) -> None:
        self.socket_path = socket_path or default_socket_path()
        self.timeout = timeout

    def _connect(self) -> socket.socket:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        try:
            s.connect(self.socket_path)
        except OSError as e:
            s.close()
            raise HsedError(
                f"can't reach hsed at {self.socket_path} ({e}). "
                f"Start the daemon first, e.g.: sudo hsedd  (or `systemctl start hsed`)"
            ) from e
        return s

    def ping(self) -> bool:
        s = self._connect()
        try:
            s.sendall(b"PING\n")
            line = _LineReader(s).readline()
            return line is not None and json.loads(line).get("type") == "pong"
        finally:
            s.close()

    def scan(self, min_size: int = 0, only_pid: int = 0) -> tuple[list[HiddenEntry], int]:
        s = self._connect()
        try:
            s.sendall(f"SCAN {min_size} {only_pid}\n".encode())
            reader = _LineReader(s)
            entries: list[HiddenEntry] = []
            while True:
                line = reader.readline()
                if line is None:
                    raise HsedError("connection closed unexpectedly during SCAN")
                obj = json.loads(line)
                if obj["type"] == "entry":
                    entries.append(HiddenEntry.from_json(obj))
                elif obj["type"] == "end":
                    return entries, obj["total_bytes"]
                elif obj["type"] == "error":
                    raise HsedError(obj["message"])
        finally:
            s.close()

    def truncate(self, pid: int, fd: int) -> int:
        s = self._connect()
        try:
            s.sendall(f"TRUNCATE {pid} {fd}\n".encode())
            line = _LineReader(s).readline()
            if line is None:
                raise HsedError("connection closed unexpectedly during TRUNCATE")
            obj = json.loads(line)
            if obj["type"] == "error":
                raise HsedError(obj["message"])
            return obj["freed"]
        finally:
            s.close()

    def hup(self, pid: int) -> None:
        s = self._connect()
        try:
            s.sendall(f"HUP {pid}\n".encode())
            line = _LineReader(s).readline()
            if line is None:
                raise HsedError("connection closed unexpectedly during HUP")
            obj = json.loads(line)
            if obj["type"] == "error":
                raise HsedError(obj["message"])
        finally:
            s.close()

    def kill(self, pid: int) -> None:
        """Sends SIGKILL. Unlike hup(), this is not a request the process
        can handle gracefully — it terminates immediately, with no chance
        to clean up. The kernel releases all its fds as part of normal
        process teardown, same as any other way the process could die."""
        s = self._connect()
        try:
            s.sendall(f"KILL {pid}\n".encode())
            line = _LineReader(s).readline()
            if line is None:
                raise HsedError("connection closed unexpectedly during KILL")
            obj = json.loads(line)
            if obj["type"] == "error":
                raise HsedError(obj["message"])
        finally:
            s.close()

    def open_stream(self, pid: int, fd: int) -> "StreamSession":
        """
        Opens a STREAM session against the daemon. Connecting and sending
        the command happens synchronously (it's a local socket — this
        takes microseconds), so this is safe to call directly from a UI
        thread; iterate the returned session's .events() from a worker
        thread, and call .close() from the UI thread to abort early.
        """
        s = self._connect()
        s.settimeout(None)  # a quiet fd can go a long time between writes
        try:
            s.sendall(f"STREAM {pid} {fd}\n".encode())
        except OSError as e:
            s.close()
            raise HsedError(f"could not start STREAM: {e}") from e
        return StreamSession(s)


class StreamSession:
    """
    A live STREAM connection. Call .events() (from any thread — typically
    a worker thread, since it blocks) to iterate captured writes, and
    .close() (typically from the UI thread) to abort early: closing the
    socket unblocks a recv() that's currently in progress in the other
    thread, which is what makes the tracer's poll_cb notice the
    disconnect and detach from the traced process promptly. Always call
    .close() when you're done with a session — an un-closed one leaves
    the daemon attached to the target process indefinitely.
    """

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._reader = _LineReader(sock)
        self._closed = False

    def events(self) -> Iterator[WriteEvent]:
        while True:
            try:
                line = self._reader.readline()
            except OSError:
                return  # socket closed (probably by .close() from another thread)
            if line is None:
                return
            obj = json.loads(line)
            t = obj["type"]
            if t == "attached":
                continue
            elif t == "write":
                yield WriteEvent.from_json(obj)
            elif t == "stream_end":
                return
            elif t == "error":
                raise HsedError(obj["message"])

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()
