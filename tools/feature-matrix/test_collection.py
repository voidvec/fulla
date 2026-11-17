"""Audit actual CMake source collection against social fixture dependencies."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cmake'), 'CMake required for collection regression')
class FeatureCollectionTest(unittest.TestCase):
    def test_social_fixture_sources_follow_feature_switch(self):
        source = (REPO / 'tests/CMakeLists.txt').read_text()
        block = source[source.index('file(GLOB_RECURSE UNIT_TESTS'):source.index('# Root test sources')]
        block = block.replace('${CMAKE_CURRENT_SOURCE_DIR}', (REPO / 'tests').as_posix())
        dependencies = [p.resolve() for p in (REPO / 'tests').rglob('*.cc')
                        if '"SocialMockFixture.h"' in p.read_text()]
        self.assertTrue(dependencies)
        with tempfile.TemporaryDirectory(prefix='feature-collection-') as tmp:
            root = Path(tmp)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.16)\nproject(Collection NONE)\n' + block +
                '\nfile(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${INTEGRATION_TESTS}")\n')
            for social in ('OFF', 'ON'):
                result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                                         '-DWITH_SOCIAL=' + social, '-DWITH_WEBAUTHN=ON'],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                sources = {Path(p).resolve() for p in (root / 'build/sources.txt').read_text().split(';')}
                for path in dependencies:
                    self.assertEqual(path in sources, social == 'ON', str(path))
                self.assertIn((REPO / 'tests/integration/oidc/ConsentAuthGateHttpTest.cc').resolve(), sources)


if __name__ == '__main__':
    unittest.main()
