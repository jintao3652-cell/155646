"""机型库搜索（设置对话框自动补全）回归测试。

候选来自 aircraft_db.search()：只返回库内合法 designator，
避免用户手输错误代码导致 equipment 上报无效。
"""
import unittest

from xfly_client import aircraft_db


class TestAircraftSearch(unittest.TestCase):

    def test_code_prefix(self):
        """代码前缀：A32 应给出 A320/A321 系列及 PA32。"""
        rows = aircraft_db.search("A32", 12)
        codes = {r["designator"] for r in rows}
        self.assertIn("A320", codes)
        self.assertIn("A321", codes)
        self.assertIn("PA32", codes)

    def test_exact_code_first(self):
        """精确代码查询：C919 的首条应是 C919，且厂商为 COMAC。"""
        rows = aircraft_db.search("C919", 5)
        self.assertTrue(rows)
        self.assertEqual(rows[0]["designator"], "C919")
        self.assertEqual(rows[0]["manufacturer"], "COMAC")

    def test_model_keyword(self):
        """型号关键词：boeing 747 应给出 B74x（BOEING）。"""
        rows = aircraft_db.search("boeing 747", 6)
        self.assertTrue(rows)
        self.assertTrue(all(r["manufacturer"] == "BOEING" for r in rows))
        self.assertTrue(any(r["designator"].startswith("B74") for r in rows))

    def test_cherokee_keyword(self):
        """型号名：cherokee 应命中 PA-32 Cherokee。"""
        rows = aircraft_db.search("cherokee", 6)
        codes = {r["designator"] for r in rows}
        self.assertIn("PA32", codes)

    def test_no_match_returns_empty(self):
        """无匹配词返回空列表，不报错。"""
        self.assertEqual(aircraft_db.search("zzzznotexist", 10), [])

    def test_empty_query(self):
        self.assertEqual(aircraft_db.search(""), [])

    def test_limit_respected(self):
        rows = aircraft_db.search("A", 3)
        self.assertLessEqual(len(rows), 3)

    def test_fields_present(self):
        """候选字段需覆盖 UI 展示的 code/厂商/型号/WTC。"""
        rows = aircraft_db.search("A320", 3)
        self.assertTrue(rows)
        for key in ("designator", "model", "manufacturer", "wtc",
                    "engines", "engine_type", "combined_type"):
            self.assertIn(key, rows[0])


if __name__ == "__main__":
    unittest.main()
