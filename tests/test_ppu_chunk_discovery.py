"""Synthetic tests for scalable, table-safe PPU source discovery.

Runs without PS3 game files or native game execution.
"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    path = ROOT / "tools" / name
    spec = importlib.util.spec_from_file_location(name[:-3], path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ChunkDiscoveryTests(unittest.TestCase):
    def test_variable_chunk_count_and_table_exclusion(self):
        filenames = (
            "ppu_recomp_000.cpp", "ppu_recomp_009.cpp",
            "ppu_recomp_100.cpp", "ppu_recomp_223.cpp",
            "ppu_recomp_999.cpp", "ppu_recomp_1000.cpp",
            "ppu_recomp_table.cpp", "ppu_recomp_0extra.cpp",
            "ppu_recomp_223.cpp.backup", "other.cpp"
        )
        with tempfile.TemporaryDirectory() as tmp:
            for filename in filenames:
                (Path(tmp) / filename).write_text("// synthetic\n", encoding="utf8")
            expected = sorted(str(Path(tmp) / n) for n in filenames[:6])
            for script in ("regen_common.py", "ppu_hooks.py"):
                with self.subTest(script=script):
                    found = load_tool(script).ppu_chunks(tmp)
                    self.assertEqual(found, expected)

    def test_hook_application_in_high_numbered_chunk(self):
        # Synthetic C++ only: ensure a hook in chunk 223 is applied, not lost
        # because older tools only discovered files beginning with "0".
        import json
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "ppu_recomp_000.cpp").write_text(
                '#include "ppu_recomp.h"\nvoid func_00000001(ppu_context* ctx) {\n  untouched;\n}\n',
                encoding="utf8")
            target = directory / "ppu_recomp_223.cpp"
            target.write_text(
                '#include "ppu_recomp.h"\nvoid func_1234ABCD(ppu_context* ctx) {\n  old_value;\n}\n',
                encoding="utf8")
            (directory / "ppu_recomp_table.cpp").write_text(
                "// function table should not be modified\n", encoding="utf8")
            hooks = directory / "hooks.json"
            hooks.write_text(json.dumps({
                "funcs": {"func_1234ABCD": [{
                    "at": 1, "ctx": "void func_1234ABCD(ppu_context* ctx) {",
                    "rm": ["  old_value;"], "add": ["  fixed_value;"]
                }]},
                "preamble": [], "header": None
            }), encoding="utf8")
            load_tool("ppu_hooks.py").apply(str(directory), str(hooks))
            self.assertIn("fixed_value;", target.read_text(encoding="utf8"))
            self.assertNotIn("old_value;", target.read_text(encoding="utf8"))
            self.assertIn("function table", (directory / "ppu_recomp_table.cpp").read_text())
    def test_release_windowed_resolution(self):
        # This choice is for Windowed mode, not 5120x2160 internal rendering.
        data = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf8"))
        options = next(x for x in data["settings"] if x["id"] == "window")["choices"]
        self.assertIn("5120x2160", [x["set"]["window"] for x in options])
        cpp = (ROOT / "project" / "launcher.cpp").read_text(encoding="utf8")
        self.assertIn('"5120x2160"', cpp)

    def test_spu_worker_bound(self):
        code = (ROOT / "tools" / "r3_regen.py").read_text(encoding="utf8")
        self.assertEqual(code.count("ThreadPoolExecutor(min(4, os.cpu_count() or 4))"), 2)


if __name__ == "__main__":
    unittest.main()
