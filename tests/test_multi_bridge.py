"""验证桥接客户端的多端点自动探测：XP / MSFS 任一在线即可连上。

这是"三个模拟器任一在线即可连线 FSD"的核心保证：客户端不能只盯 51001，
必须同时探测 XConnect 与 MConnect（含退避端口），任一在线即 connected=True。
"""
import socket
import threading
import unittest

from xfly_client.xconnect.client import (
    MCONNECT_FALLBACK_PORTS,
    XConnectClient,
    build_bridge_ports,
)


def _start_fake_bridge(port_holder, sim, plugin):
    """在临时端口起一个假桥：accept 后发 connected 握手，然后保持连接。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port_holder["port"] = srv.getsockname()[1]

    def serve():
        try:
            conn, _ = srv.accept()
            with conn:
                conn.sendall(
                    ('{"type":"connected","protocol":1,"plugin":"%s",'
                     '"version":"test","sim":"%s",'
                     '"capabilities":["csl","parts"]}\n' % (plugin, sim))
                    .encode("utf-8"))
                conn.settimeout(0.2)
                while True:
                    try:
                        if not conn.recv(4096):
                            break
                    except OSError:
                        break
        except OSError:
            pass
        finally:
            srv.close()

    threading.Thread(target=serve, daemon=True).start()
    return port_holder["port"]


class TestBuildBridgePorts(unittest.TestCase):
    def test_covers_xp_and_msfs_without_duplicates(self):
        ports = build_bridge_ports(51001, 51002)
        self.assertEqual(ports[0], 51001, "XConnect 应为首选端口")
        self.assertIn(51002, ports)
        for p in MCONNECT_FALLBACK_PORTS:
            self.assertIn(p, ports)
        self.assertEqual(len(ports), len(set(ports)), "候选端口不应重复")

    def test_custom_mconnect_port_kept(self):
        ports = build_bridge_ports(51001, 51009)
        self.assertIn(51009, ports)
        self.assertEqual(ports[0], 51001)

    def test_port_setter_rebuilds_candidates(self):
        c = XConnectClient("127.0.0.1", 51001, mconnect_port=51007)
        self.assertIn(51007, c.ports)
        c.port = 51001
        self.assertIn(51007, c.ports, "设置 port 后应保留 MConnect 候选端口")


class TestMultiEndpointConnect(unittest.TestCase):
    def _connect_to(self, sim, plugin):
        holder = {}
        port = _start_fake_bridge(holder, sim, plugin)
        ready = threading.Event()
        # 首选端口 51001 故意指向一个无人监听的端口，强制客户端回退探测
        c = XConnectClient("127.0.0.1", port, ports=[59999, port])
        c.on_handshake = lambda doc: ready.set()
        c.start()
        try:
            self.assertTrue(ready.wait(3.0), "未在回退端口上完成握手")
            self.assertTrue(c.connected)
            self.assertEqual(c.active_port, port)
            self.assertEqual(c.plugin, plugin)
            self.assertEqual(c.sim, sim)
        finally:
            c.stop()
        return c

    def test_msfs_bridge_online(self):
        c = self._connect_to("msfs", "MConnect")
        # MC 必须被分类为独立的 mconnect 桥，不能归入 xconnect
        self.assertEqual(c.bridge_kind, "mconnect")

    def test_xplane_bridge_online(self):
        c = self._connect_to("xplane", "XConnect")
        self.assertEqual(c.bridge_kind, "xconnect")

    def test_kind_classified_by_plugin_name_overrides_port(self):
        """握手插件名优先于端口推断：端口表猜错时以插件名为准。"""
        holder = {}
        # 用非 MConnect 端口（伪装成 51001 段）跑 MConnect 握手
        port = _start_fake_bridge(holder, "msfs", "MConnect")
        ready = threading.Event()
        c = XConnectClient("127.0.0.1", 51001, ports=[port])
        c.on_handshake = lambda doc: ready.set()
        c.start()
        try:
            self.assertTrue(ready.wait(3.0), "未完成握手")
            self.assertEqual(c.bridge_kind, "mconnect",
                             "插件名含 MConnect 时必须分类为 mconnect")
        finally:
            c.stop()

    def test_kind_classified_by_port_before_handshake(self):
        """连上后（握手前）即可按端口给出 provisional 分类。"""
        c = XConnectClient("127.0.0.1", 51001, mconnect_port=51002)
        self.assertEqual(c._classify_port(51001), "xconnect")
        self.assertEqual(c._classify_port(51002), "mconnect")
        self.assertEqual(c._classify_port(51003), "mconnect")
        self.assertEqual(c._classify_port(51004), "mconnect")
        self.assertEqual(c.bridge_kind, "", "未连接前分类应为空")

    def test_falls_back_when_primary_bridge_offline(self):
        """首选端口无服务时，应回退连到另一个在线端点（如 XP 关、MSFS 开）。"""
        holder2 = {}
        p2 = _start_fake_bridge(holder2, "msfs", "MConnect")
        ready = threading.Event()
        # 首选端口 59998 无人监听，唯一在线的是 p2（模拟 MSFS 单独在线）
        c = XConnectClient("127.0.0.1", 59998, ports=[59998, p2])
        c.on_handshake = lambda doc: ready.set()
        c.start()
        try:
            self.assertTrue(ready.wait(3.0), "未回退到备用端点")
            self.assertTrue(c.connected)
            self.assertEqual(c.active_port, p2)
            self.assertEqual(c.plugin, "MConnect")
        finally:
            c.stop()


if __name__ == "__main__":
    unittest.main()
