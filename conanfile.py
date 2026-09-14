"""Fulla Conan dependency descriptor.

Replaces the legacy conanfile.txt (see design.md §9.1). This file is the
single, three-platform (Linux/Windows/macOS) source of truth for Fulla's
C++ dependencies.

Notable options beyond plain dependency pinning:
  - with_identity / with_social / with_webauthn: gate optional Fulla
    feature areas (identity SDK, social login controllers, WebAuthn/FIDO2)
    that will be wired up to conditional compilation in later milestones
    (F9). They default to True to preserve today's build behavior; setting
    them to False lets a consumer shrink the dependency surface once the
    corresponding milestone (M2.5/M5) lands the conditional compilation.
  - with_webauthn additionally pulls in the extra crypto dependencies
    FIDO2 needs (declared explicitly rather than left as a transitive
    surprise). NOTE: a real WebAuthn implementation will require CBOR
    decoding for attestation/assertion objects, but the current controller
    is a non-cryptographic stub (see WebAuthnService.h:44-56) and consumes
    no CBOR; the libcbor dependency was removed as dead (zero #include).
    It should be re-added when real WebAuthn crypto lands.
"""

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps


class FullaConan(ConanFile):
    name = "fulla"
    version = "1.2.0"
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
        "drogon/*:with_orm": True,
        "drogon/*:with_postgres": True,
        "drogon/*:with_redis": True,
        "drogon/*:with_ctl": True,
        "drogon/*:with_sqlite": True,
        "drogon/*:with_brotli": True,

        # --- libcurl: keep a single TLS stack (OpenSSL) across the project ---
        "libcurl/*:with_ssl": "openssl",
    }

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
