"""Fulla Conan dependency descriptor.

Dependencies are pinned here and in conan.lock for Linux, Windows and macOS.
with_social and with_webauthn map to CMake's WITH_* feature switches.
with_identity=False is reserved but unsupported: the product and Postgres
adapter still require the identity package, so validate() rejects it explicitly.

WebAuthn performs real CBOR/COSE and ES256 verification. libcbor remains an
unconditional requirement because the identity build compiles WebAuthnCrypto
and its tests even when the WebAuthn service/controllers are disabled.
"""

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMakeToolchain, CMakeDeps


class FullaConan(ConanFile):
    name = "fulla"
    version = "1.6.0"
    settings = "os", "compiler", "build_type", "arch"

    options = {
        "with_identity": [True, False],
        "with_social": [True, False],
        "with_webauthn": [True, False],
    }
    default_options = {
        "with_identity": True,
        "with_social": True,
        "with_webauthn": True,

        # --- drogon options (M0 Task 1) ---
        # with_boost=False: drogon 1.9.x upstream no longer uses Boost
        # (no Boost reference anywhere in its CMake); the conancenter
        # recipe's True default only drags boost/1.83.0 into the graph
        # (and its Boost::test_exec_monitor main() into MSVC link lines).
        "drogon/*:with_boost": False,
        "drogon/*:with_orm": True,
        "drogon/*:with_postgres": True,
        "drogon/*:with_redis": True,
        "drogon/*:with_ctl": True,
        "drogon/*:with_sqlite": True,
        "drogon/*:with_brotli": True,

        # --- libcurl: keep a single TLS stack (OpenSSL) across the project ---
        "libcurl/*:with_ssl": "openssl",
    }

    def validate(self):
        if not self.options.with_identity:
            raise ConanInvalidConfiguration(
                "with_identity=False is not supported: the product and Postgres "
                "adapter require fulla::identity. Keep with_identity=True; use "
                "with_social and with_webauthn to disable their compiled surfaces."
            )

    def requirements(self):
        self.requires("drogon/1.9.13")
        self.requires("openssl/3.5.7", override=True)
        self.requires("jsoncpp/1.9.5")
        self.requires("hiredis/1.2.0")
        self.requires("libcurl/8.6.0")
        self.requires("brotli/1.1.0")
        self.requires("zlib/1.3.1", override=True)
        # #142: real WebAuthn attestation/assertion verification. libcbor
        # backs the typed read-only CborReader in libs/identity/src/webauthn/.
        self.requires("libcbor/0.13.0")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        tc = CMakeToolchain(self)
        # M5 Task 31 (F9): thread the Conan with_* options through to the
        # CMake-side WITH_* cache variables the build actually consumes
        # (option() defaults live in the top-level CMakeLists.txt +
        # libs/identity/CMakeLists.txt; without this mapping, `conan build
        # -o with_webauthn=False` would NOT disable WebAuthn compilation).
        tc.variables["WITH_IDENTITY"] = bool(self.options.with_identity)
        tc.variables["WITH_SOCIAL"] = bool(self.options.with_social)
        tc.variables["WITH_WEBAUTHN"] = bool(self.options.with_webauthn)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()
