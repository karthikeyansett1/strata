import tempfile
import unittest
from pathlib import Path

import numpy as np
import strata


class BindingTest(unittest.TestCase):
    def test_kv_round_trip_and_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "db"
            db = strata.DB(path)
            db.put("a", "first")
            db.put("b", "second")
            db.put(b"raw", b"\x00\xff")
            self.assertEqual(db.get("raw", raw=True), b"\x00\xff")
            self.assertEqual(db.scan("a", "c"), [("a", "first"), ("b", "second")])
            db.delete("a")
            self.assertIsNone(db.get("a"))
            del db

            db = strata.DB(path)
            self.assertIsNone(db.get("a"))
            self.assertEqual(db.get("b"), "second")
            self.assertEqual(db.scan(b"raw", b"s", raw=True), [(b"raw", b"\x00\xff")])
            del db

    def test_vector_updates_validate_and_survive_reopen(self):
        with tempfile.TemporaryDirectory() as directory:
            db = strata.DB(directory, metric=strata.Metric.L2)
            self.assertEqual(db.vsearch([1, 0], 10), [])
            db.put("v/red", "ordinary key")
            db.vput("red", np.array([9, 0], dtype=np.float32))
            db.vput("blue", [6, 8])
            db.vput("red", [3, 4])
            for vector in ([1], [float("nan"), 0], []):
                with self.assertRaises(ValueError):
                    db.vput("invalid", vector)
            with self.assertRaises(ValueError):
                db.vsearch([0, 0], 0)
            self.assertEqual(db.vsearch([0, 0], 4), [("red", 5.0), ("blue", 10.0)])
            del db

            db = strata.DB(directory, metric=strata.Metric.L2)
            self.assertEqual(db.vsearch([0, 0], 1, ef_search=20), [("red", 5.0)])
            self.assertEqual(db.get("v/red"), "ordinary key")
            del db

    def test_numpy_benchmark_paths_agree_on_small_dataset(self):
        source = np.array([[0, 1], [1, 0], [-1, 0], [0, -1]], dtype=np.float32)
        queries = np.array([[2, 0]], dtype=np.float32)
        flat = strata._FlatIndex(strata.Metric.COSINE)
        graph = strata._HnswIndex(strata.Metric.COSINE, 4, 20)
        flat.add(source)
        graph.add(source)
        np.testing.assert_array_equal(flat.search(queries, 3), graph.search(queries, 3))
        with self.assertRaises(ValueError):
            graph.search(np.zeros((1, 3), dtype=np.float32), 1)


if __name__ == "__main__":
    unittest.main()
