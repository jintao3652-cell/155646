import socket
import threading
import time
import unittest

from xfly_client.fsd.link import FsdLink


class TestFsdLinkMockServer(unittest.TestCase):
    """半包/粘包：服务端分两次发送，客户端必须拼成完整行。"""

    def test_line_framing(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        done = threading.Event()

        def serve():
            try:
                conn, _ = srv.accept()
                with conn:
                    conn.sendall(b"#TMABC:SERVER:hello")
                    time.sleep(0.05)
                    conn.sendall(
                        b"\n#SBABC:ALL:VI:31.2:121.3:47:12:2\n")
                    done.wait(2.0)
            finally:
                srv.close()

        threading.Thread(target=serve, daemon=True).start()

        lines = []
        got = threading.Event()

        def on_line(line):
            lines.append(line)
            if len(lines) >= 2:
                got.set()

        link = FsdLink("127.0.0.1", port)
        link.on_line = on_line
        link.connect()
        try:
            self.assertTrue(got.wait(3.0), "lines not received")
            self.assertEqual(len(lines), 2)
            self.assertTrue(lines[0].startswith("#TMABC"))
            self.assertTrue(lines[1].startswith("#SBABC:ALL:VI:"))
        finally:
            done.set()
            link.close()


if __name__ == "__main__":
    unittest.main()
