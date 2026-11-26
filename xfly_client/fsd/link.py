"""FSD 底层 TCP 链路：GB18030 编码、CRLF 行分帧、发送锁、断线回调。
"""

from __future__ import annotations

import codecs
import socket
import threading
from typing import Callable, Optional

DEFAULT_PORT = 6809
SERVER_CALLSIGN = "SERVER"


class FsdLink:
    def __init__(self, host: str, port: int = DEFAULT_PORT) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = None
        self._rx = ""
        self._send_lock = threading.Lock()
        self._stop = threading.Event()
        self._rx_thread: Optional[threading.Thread] = None

        self.on_line: Optional[Callable[[str], None]] = None
        self.on_disconnected: Optional[Callable[[], None]] = None
        self.on_error: Optional[Callable[[Exception], None]] = None

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self, timeout: float = 5.0) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=timeout)
        sock.settimeout(1.0)
        self._sock = sock
        self._rx = ""
        self._stop.clear()
        self._rx_thread = threading.Thread(
            target=self._receive_loop, name="fsd-rx", daemon=True)
        self._rx_thread.start()

    def send(self, line: str) -> None:
        sock = self._sock
        if sock is None:
            raise RuntimeError("not connected")
        if not line.endswith("\n"):
            line += "\r\n"
        payload = line.encode("gb18030", errors="replace")
        with self._send_lock:
            sock.sendall(payload)

    def close(self) -> None:
        self._stop.set()
        sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    # ---------- internals ----------

    def _receive_loop(self) -> None:
        sock = self._sock
        # errors="replace"：服务器偶发非法字节不应导致整条连接被断开
        decoder = codecs.getincrementaldecoder("gb18030")(errors="replace")
        try:
            while not self._stop.is_set() and sock is not None:
                try:
                    data = sock.recv(8192)
                except socket.timeout:
                    continue
                except OSError as exc:
                    if not self._stop.is_set():
                        self._emit_error(exc)
                    break
                if not data:
                    break
                text = decoder.decode(data)
                self._rx += text
                while True:
                    idx = self._rx.find("\n")
                    if idx < 0:
                        break
                    line = self._rx[:idx].rstrip("\r")
                    self._rx = self._rx[idx + 1:]
                    if line:
                        try:
                            if self.on_line:
                                self.on_line(line)
                        except Exception as exc:  # 回调异常不得杀死接收线程
                            self._emit_error(exc)
        except Exception as exc:
            if not self._stop.is_set():
                self._emit_error(exc)
        finally:
            self.close()
            if self.on_disconnected:
                try:
                    self.on_disconnected()
                except Exception:
                    pass

    def _emit_error(self, exc: Exception) -> None:
        if self.on_error:
            try:
                self.on_error(exc)
            except Exception:
                pass
