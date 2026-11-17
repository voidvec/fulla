"""Compile the real expected-route helper without Drogon or a running server."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('g++'), 'C++17 compiler required for route subset probe')
class ExpectedRouteTest(unittest.TestCase):
    def test_upstream_routes_follow_social_but_shared_routes_remain(self):
        source = r'''
#include "EnabledFeatureRoutes.h"
#include <cassert>
int main() {
    using namespace fulla::test::features;
    const std::string baseline =
        "GET /api/admin/upstream-providers\n"
        "POST /api/admin/upstream-providers/key\n"
        "POST /api/auth/oidc/provider/login\n"
        "GET /api/auth/providers\n"
        "POST /api/auth/providers/github/authorize\n"
        "POST /oauth2/consent\n"
        "GET /api/admin/upstream-providers-archive\n";
#ifdef WITH_SOCIAL
    assert(routeEnabled("/api/admin/upstream-providers"));
    assert(routeEnabled("/api/auth/oidc/provider/login"));
    assert(expectedRoutes(baseline) == baseline);
#else
    assert(!routeEnabled("/api/admin/upstream-providers"));
    assert(!routeEnabled("/api/admin/upstream-providers/key"));
    assert(!routeEnabled("/api/auth/oidc/provider/login"));
    assert(expectedRoutes(baseline) ==
        "GET /api/auth/providers\n"
        "POST /api/auth/providers/github/authorize\n"
        "POST /oauth2/consent\n"
        "GET /api/admin/upstream-providers-archive\n");
#endif
    assert(routeEnabled("/api/auth/providers"));
    assert(routeEnabled("/api/auth/providers/github/authorize"));
    assert(routeEnabled("/oauth2/consent"));
    assert(routeEnabled("/api/admin/upstream-providers-archive"));
}
'''
        with tempfile.TemporaryDirectory(prefix='feature-routes-') as tmp:
            root = Path(tmp)
            (root / 'probe.cc').write_text(source)
            for social in (False, True):
                exe = root / ('probe-on' if social else 'probe-off')
                command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DWITH_WEBAUTHN',
                           '-I', str(REPO / 'tests/common'), str(root / 'probe.cc'), '-o', str(exe)]
                if social:
                    command.append('-DWITH_SOCIAL')
                built = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                ran = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
