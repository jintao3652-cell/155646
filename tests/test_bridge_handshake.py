import socket
import threading
import unittest

from xfly_client.version import BRIDGE_PROTOCOL
from xfly_client.xconnect.client import XConnectClient


class TestBridgeHandshake(unittest.TestCase):
    def test_connected_handshake(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        got = {}
        hello_evt = threading.Event()
        done = threading.Event()

        def serve():
            try:
                conn, _ = srv.accept()
                with conn:
                    conn.sendall(
                        b'{"type":"connected","protocol":1,'
                        b'"plugin":"XConnect","version":"1.0.0",'
                        b'"capabilities":["csl","parts"]}\n')
                    conn.settimeout(2.0)
                    try:
                        got["hello"] = conn.recv(4096).decode(
                            "utf-8", "replace")
                    except OSError:
                        pass
                    hello_evt.set()
                    done.wait(2.0)
            finally:
                srv.close()

        t = threading.Thread(target=serve, daemon=True)
        t.start()

        client = XConnectClient("127.0.0.1", port)
        ready = threading.Event()
        client.on_handshake = lambda doc: ready.set()
        client.start()
        try:
            self.assertTrue(ready.wait(3.0), "handshake not received")
            self.assertTrue(client.handshake_ok)
            self.assertEqual(client.plugin, "XConnect")
            self.assertEqual(client.plugin_version, "1.0.0")
            self.assertEqual(client.protocol_version, BRIDGE_PROTOCOL)
            self.assertIn("csl", client.capabilities)
            self.assertTrue(hello_evt.wait(2.0), "client hello not received")
            self.assertIn('"hello"', got.get("hello", ""))
            self.assertIn('"XFlySim-Client"', got.get("hello", ""))
        finally:
            client.stop()
            done.set()


if __name__ == "__main__":
    unittest.main()
