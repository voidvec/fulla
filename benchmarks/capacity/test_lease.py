"""Reject diagnostic Release artifacts before generating benchmark credentials."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from lease import create


class LeaseTests(unittest.TestCase):
    def test_sanitizer_and_coverage_release_builds_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "server"
            binary.write_bytes(b"fixture-binary")
            cache = root / "CMakeCache.txt"
            args = SimpleNamespace(
                product_commit="a" * 40,
                build_cache=cache,
                binary=binary,
                db_port=25433,
                redis_port=16380,
                app_port=15555,
                db_proxy_port=25432,
                redis_container="fulla-s07-test",
                output=root / "fixture.json",
                sql_output=root / "provision.sql",
            )
            base = "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\nFULLA_WERROR:BOOL=ON\n"
            for diagnostic in (
                "FULLA_SANITIZER:STRING=address\n",
                "FULLA_TEST_COVERAGE:BOOL=ON\n",
                "CMAKE_CXX_FLAGS:STRING=-fsanitize=thread\n",
                "CMAKE_EXE_LINKER_FLAGS:STRING=--coverage\n",
            ):
                cache.write_text(base + diagnostic)
                with patch("lease.subprocess.run"), self.assertRaises(ValueError):
                    create(args)
                self.assertFalse(args.output.exists())
                self.assertFalse(args.sql_output.exists())


if __name__ == "__main__":
    unittest.main()
