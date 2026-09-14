#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/storage/memory/MemoryRepositoryBundle.h>
#include <fulla/oauth2/repository/IGrantRepository.h>
#include <fulla/oauth2/model/Dto.h>
#include <future>
#include <iostream>

// Phase 4.7b (fulla-sdk-refactor): migrated off the god MemoryOAuth2Storage
// to the per-backend MemoryRepositoryBundle. Auth-code operations now go
// through bundle.grantRepository() (NEW IGrantRepository + model::OAuth2AuthCode).
// The redirect_uri atomicity contract (RFC 6749 §4.1.3) is unchanged.

using AuthCode = fulla::oauth2::model::OAuth2AuthCode;
using GrantRepo = fulla::oauth2::repository::IGrantRepository;

namespace
{
std::shared_ptr<GrantRepo> makeSeededGrantRepo(const Json::Value &clientsConfig)
{
    auto bundle = std::make_shared<fulla::storage::memory::MemoryRepositoryBundle>();
    bundle->initFromConfig(clientsConfig);
    return bundle->grantRepository();
}

Json::Value vueClientConfig()
{
    Json::Value config;
    config["fulla-portal"]["secret"] = "test-secret";
    config["fulla-portal"]["redirect_uri"] = "http://localhost:5173/callback";
    config["fulla-portal"]["client_type"] = "public";
    return config;
}
}  // namespace

DROGON_TEST(Unit_P1_RedirectUri_MemoryStorage_Works)
{
    LOG_INFO << "=== Integration Test: Redirect URI Validation (Memory) ===";

    auto grant = makeSeededGrantRepo(vueClientConfig());

    AuthCode testCode;
    testCode.code = "test_memory_redirect";
    testCode.clientId = "fulla-portal";
    testCode.userId = "test_user";
    testCode.expiresAt = std::time(nullptr) + 3600;
    testCode.used = false;
    testCode.redirectUri = "http://localhost:5173/callback";

    try
    {
        // Test 1: Save auth code with redirect URI
        LOG_INFO << "--- Test 1: Save auth code with redirect URI ---";
        {
            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();
            LOG_INFO << "Auth code saved with redirect URI: " << testCode.redirectUri;
        }

        // Test 2: Valid redirect URI - should succeed
        LOG_INFO << "--- Test 2: Valid redirect URI ---";
        {
            std::promise<std::optional<AuthCode>> p;
            auto f = p.get_future();
            grant->consumeAuthCode(
              testCode.code, testCode.redirectUri, [&](std::optional<AuthCode> code) {
                  p.set_value(code);
              }
            );
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f.get();
            CHECK(result.has_value());
            CHECK(result->redirectUri == testCode.redirectUri);
            LOG_INFO << "Valid redirect URI accepted";
        }

        // Test 3: Save another auth code for invalid redirect URI test
        LOG_INFO << "--- Test 3: Setup for invalid redirect URI test ---";
        testCode.code = "test_memory_invalid";
        {
            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();
            LOG_INFO << "Auth code saved for invalid redirect URI test";
        }

        // Test 4: Invalid redirect URI - should fail
        LOG_INFO << "--- Test 4: Invalid redirect URI ---";
        {
            std::promise<std::optional<AuthCode>> p;
            auto f = p.get_future();
            grant->consumeAuthCode(
              testCode.code,
              "http://malicious-site.com/callback",
              [&](std::optional<AuthCode> code) { p.set_value(code); }
            );
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f.get();
            CHECK(!result.has_value());
            LOG_INFO << "Invalid redirect URI properly rejected";
        }

        LOG_INFO << "=== Memory Storage Redirect URI Validation Test Completed ===";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Test Failed: " << e.what();
        throw;
    }
}

DROGON_TEST(Unit_P1_RedirectUri_Atomicity_Works)
{
    LOG_INFO << "=== Integration Test: Redirect URI Validation Atomicity ===";

    auto grant = makeSeededGrantRepo(vueClientConfig());

    AuthCode testCode;
    testCode.code = "test_atomic_" + std::string(4, 'y');
    testCode.clientId = "fulla-portal";
    testCode.userId = "test_user";
    testCode.expiresAt = std::time(nullptr) + 3600;
    testCode.used = false;
    testCode.redirectUri = "http://localhost:5173/callback";

    try
    {
        // Test: Ensure atomic operation - invalid redirect_uri should not
        // consume code
        LOG_INFO << "--- Test: Atomicity of redirect_uri validation ---";
        {
            // Save auth code
            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();
            LOG_INFO << "Auth code saved";
        }

        // Try to consume with invalid redirect URI
        {
            std::promise<std::optional<AuthCode>> p;
            auto f = p.get_future();
            grant->consumeAuthCode(
              testCode.code,
              "http://malicious-site.com/callback",
              [&](std::optional<AuthCode> code) { p.set_value(code); }
            );
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f.get();
            CHECK(!result.has_value());
            LOG_INFO << "Invalid redirect URI rejected";
        }

        // Verify code is still available (not consumed)
        {
            std::promise<std::optional<AuthCode>> p;
            auto f = p.get_future();
            grant->getAuthCode(testCode.code, [&](std::optional<AuthCode> code) {
                p.set_value(code);
            });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Get auth code timeout");
            }
            auto result = f.get();
            CHECK(result.has_value());
            CHECK(result->used == false);
            LOG_INFO << "Auth code still available (not consumed by invalid "
                        "redirect_uri)";
        }

        // Now consume with valid redirect URI
        {
            std::promise<std::optional<AuthCode>> p;
            auto f = p.get_future();
            grant->consumeAuthCode(
              testCode.code, testCode.redirectUri, [&](std::optional<AuthCode> code) {
                  p.set_value(code);
              }
            );
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f.get();
            CHECK(result.has_value());
            CHECK(result->used == true);
            LOG_INFO << "Auth code consumed with valid redirect URI";
        }

        LOG_INFO << "=== Atomicity Test Completed ===";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Test Failed: " << e.what();
        throw;
    }
}

DROGON_TEST(Unit_P1_RedirectUri_EdgeCases_Works)
{
    LOG_INFO << "=== Integration Test: Redirect URI Validation Edge Cases ===";

    auto grant = makeSeededGrantRepo(vueClientConfig());

    try
    {
        // Test 1: Empty redirect URI
        LOG_INFO << "--- Test 1: Empty redirect URI ---";
        {
            AuthCode testCode;
            testCode.code = "test_empty_redirect";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(testCode.code, "", [&](std::optional<AuthCode> code) {
                p2.set_value(code);
            });
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f2.get();
            CHECK(result.has_value());
            LOG_INFO << "Empty redirect URI handled";
        }

        // Test 2: Case sensitivity
        LOG_INFO << "--- Test 2: Case sensitivity ---";
        {
            AuthCode testCode;
            testCode.code = "test_case_sensitive";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "http://localhost:5173/callback";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(
              testCode.code,
              "http://localhost:5173/CALLBACK",  // Different case
              [&](std::optional<AuthCode> code) { p2.set_value(code); }
            );
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            f2.get();
            // Case sensitivity depends on implementation
            LOG_INFO << "Case sensitivity test completed";
        }

        // Test 3: URL fragments
        LOG_INFO << "--- Test 3: URL fragments ---";
        {
            AuthCode testCode;
            testCode.code = "test_url_fragment";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "http://localhost:5173/callback";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(
              testCode.code,
              "http://localhost:5173/callback#fragment",  // With fragment
              [&](std::optional<AuthCode> code) { p2.set_value(code); }
            );
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            f2.get();
            // Fragment handling depends on implementation
            LOG_INFO << "URL fragment test completed";
        }

        LOG_INFO << "=== Edge Cases Test Completed ===";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Test Failed: " << e.what();
        throw;
    }
}

DROGON_TEST(Unit_P1_RedirectUri_SecurityScenarios_Works)
{
    LOG_INFO << "=== Integration Test: Redirect URI Validation Security "
                "Scenarios ===";

    auto grant = makeSeededGrantRepo(vueClientConfig());

    try
    {
        // Test 1: Open redirect attack prevention
        LOG_INFO << "--- Test 1: Open redirect attack prevention ---";
        {
            AuthCode testCode;
            testCode.code = "test_open_redirect";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "http://localhost:5173/callback";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            // Try to redirect to arbitrary domain
            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(
              testCode.code, "http://evil.com/callback", [&](std::optional<AuthCode> code) {
                  p2.set_value(code);
              }
            );
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f2.get();
            CHECK(!result.has_value());
            LOG_INFO << "Open redirect attack prevented";
        }

        // Test 2: URL traversal attack prevention
        LOG_INFO << "--- Test 2: URL traversal attack prevention ---";
        {
            AuthCode testCode;
            testCode.code = "test_url_traversal";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "http://localhost:5173/callback";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            // Try URL traversal
            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(
              testCode.code,
              "http://localhost:5173/../evil/callback",
              [&](std::optional<AuthCode> code) { p2.set_value(code); }
            );
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f2.get();
            CHECK(!result.has_value());
            LOG_INFO << "URL traversal attack prevented";
        }

        // Test 3: Null byte injection prevention
        LOG_INFO << "--- Test 3: Null byte injection prevention ---";
        {
            AuthCode testCode;
            testCode.code = "test_null_byte";
            testCode.clientId = "fulla-portal";
            testCode.userId = "test_user";
            testCode.expiresAt = std::time(nullptr) + 3600;
            testCode.used = false;
            testCode.redirectUri = "http://localhost:5173/callback";

            std::promise<void> p;
            auto f = p.get_future();
            grant->saveAuthCode(testCode, [&]() { p.set_value(); });
            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Save auth code timeout");
            }
            f.get();

            // Try null byte injection (should be rejected by string handling)
            std::string maliciousUri =
              std::string("http://localhost:5173/callback") + '\0' + ".evil.com";
            std::promise<std::optional<AuthCode>> p2;
            auto f2 = p2.get_future();
            grant->consumeAuthCode(testCode.code, maliciousUri, [&](std::optional<AuthCode> code) {
                p2.set_value(code);
            });
            if (f2.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Consume auth code timeout");
            }
            auto result = f2.get();
            CHECK(!result.has_value());
            LOG_INFO << "Null byte injection prevented";
        }

        LOG_INFO << "=== Security Scenarios Test Completed ===";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Test Failed: " << e.what();
        throw;
    }
}
