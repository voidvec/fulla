"""Reconfigure an actual CMake consumer using the repository's SDK options."""
import ast
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for cache regression")
class SdkCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sdk-cache-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "prefix" / "lib" / "cmake" / "libcbor"
        self.prefix.mkdir(parents=True)
        (self.prefix / "libcborConfig.cmake").write_text("set(libcbor_FOUND TRUE)\n")
        self.consumer = self.root / "consumer"
        self.consumer.mkdir()
        (self.consumer / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(CacheConsumer NONE)\n"
            "if(REQUIRE_CBOR)\n  find_package(libcbor CONFIG REQUIRED)\nendif()\n")

    def configure(self, source, build, options):
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build), *options],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_consumer_reconfigures(self, options_for):
        for webauthn in (False, True):
            options = options_for(webauthn)
            self.configure(self.consumer, self.root / "consumer-build", [
                f"-DREQUIRE_CBOR={'ON' if webauthn else 'OFF'}",
                f"-Dlibcbor_DIR={self.prefix}", *options])
        cache = (self.root / "consumer-build" / "CMakeCache.txt").read_text()
        self.assertIn("CMAKE_DISABLE_FIND_PACKAGE_libcbor:UNINITIALIZED=FALSE", cache)

    def test_build_tree_sdk_smoke_off_to_on(self):
        # Evaluate the actual repository CMake option block, not its rewrite.
        source = (REPO / "CMakeLists.txt").read_text()
        block = source.split('    set(_sdk_smoke_gen_platform "")', 1)[1]
        block = block.split("    if(CMAKE_GENERATOR_PLATFORM)", 1)[0]
        producer = self.root / "producer"
        producer.mkdir()
        (producer / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(Options NONE)\n" + block +
            '\nfile(WRITE "${CMAKE_BINARY_DIR}/options.txt" "${_sdk_smoke_feature_options}")\n')

        def options_for(webauthn):
            build = self.root / "producer-build"
            self.configure(producer, build, [f"-DWITH_WEBAUTHN={'ON' if webauthn else 'OFF'}"])
            return [o for o in (build / "options.txt").read_text().split(";") if o]

        self.assert_consumer_reconfigures(options_for)

    def test_matrix_primary_and_installed_consumer_options_off_to_on(self):
        # Execute only the actual common-options AST, without running Conan/build.
        module = ast.parse((REPO / "tools/feature-matrix/run.py").read_text())
        main = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == "main")
        body = next(n.body for n in main.body if isinstance(n, ast.Try))
        first = next(i for i, n in enumerate(body) if isinstance(n, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == "common" for t in n.targets))
        last = next(i for i in range(first + 1, len(body))
                    if isinstance(body[i], ast.Expr) and isinstance(body[i].value, ast.Call)
                    and isinstance(body[i].value.func, ast.Name) and body[i].value.func.id == "run")
        common_code = compile(ast.Module(body=body[first:last], type_ignores=[]), "run.py common", "exec")

        def options_for(webauthn):
            scope = {"args": SimpleNamespace(release_flags=None, webauthn=webauthn),
                     "build": self.root}
            exec(common_code, scope)
            return [o for o in scope["common"] if o.startswith("-DCMAKE_DISABLE_FIND_PACKAGE_libcbor=")]

        self.assert_consumer_reconfigures(options_for)


if __name__ == "__main__":
    unittest.main()
