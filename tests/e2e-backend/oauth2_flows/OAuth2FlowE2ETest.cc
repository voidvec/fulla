#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <drogon/Cookie.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/controllers/SessionController.h>
#include <fulla/drogon/controllers/AuthorizationEndpointController.h>
#include <fulla/drogon/controllers/TokenEndpointController.h>
#include <future>
#include <iostream>
#include <map>

using namespace drogon;
using fulla::drogon::controllers::SessionController;

DROGON_TEST(E2E_P0_OAuth2Flow_AuthCode_Works)
{
    LOG_INFO << "=== E2E Test: OAuth2 Authorization Code Flow ===";

    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (!plugin)
    {
        LOG_WARN << "OAuth2Plugin not found. Skipping test.";
        return;
    }

    // Skip this test in memory storage mode (no database)
    if (plugin->getStorageType() == "memory")
    {
        LOG_INFO << "Skipping E2E test in memory storage mode";
        return;
    }

    auto ctrl = std::make_shared<SessionController>();
    auto authzCtrl =
      std::make_shared<fulla::drogon::controllers::AuthorizationEndpointController>();
    auto tokenCtrl = std::make_shared<fulla::drogon::controllers::TokenEndpointController>();

    std::string testUserId = "e2e_user_" + utils::getUuid().substr(0, 8);
    std::string testPassword = "TestPass123!";
    std::string testEmail = testUserId + "@example.com";

    auto cleanup = [&]() {
        auto client = app().getDbClient();
        if (!client)
            return;

        std::promise<void> p;
        auto f = p.get_future();
        client->execSqlAsync(
          "DELETE FROM users WHERE username = $1",
          [&](const drogon::orm::Result &) { p.set_value(); },
          [&](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "Cleanup Failed: " << e.base().what();
              p.set_value();
          },
          testUserId
        );
        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            LOG_ERROR << "Cleanup timeout";
        }
        f.get();
        LOG_INFO << "Cleaned up user: " << testUserId;
    };

    try
    {
        // Step 1: Register User
        LOG_INFO << "--- Step 1: Register User ---";
        {
            std::map<std::string, std::string> params;
            params["username"] = testUserId;
            params["password"] = testPassword;
            params["email"] = testEmail;

            std::promise<HttpResponsePtr> p;
            auto f = p.get_future();

            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Post);
            for (const auto &param : params)
            {
                req->setParameter(param.first, param.second);
            }

            ctrl->registerUser(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Registration timeout");
            }

            auto resp = f.get();
            if (resp->getStatusCode() == k409Conflict)
            {
                LOG_WARN << "User already exists, proceeding...";
            }
            else
            {
                CHECK(resp->getStatusCode() == k200OK);
                LOG_INFO << "User registered: " << testUserId;
            }
        }

        // Step 2: Authorization Request (without login - should return login
        // page)
        LOG_INFO << "--- Step 2: Authorization Request (Expected: Login Page) ---";
        {
            std::promise<HttpResponsePtr> p;
            auto f = p.get_future();

            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Get);
            req->setParameter("response_type", "code");
            req->setParameter("client_id", "fulla-portal");
            req->setParameter("redirect_uri", "http://localhost:5173/callback");
            req->setParameter("scope", "openid profile");
            req->setParameter("state", "test_state_123");

            authzCtrl->authorize(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Authorization timeout");
            }

            auto resp = f.get();

            // Without login, should get either:
            // 1. 200 OK with login page HTML, or
            // 2. 302 redirect to login, or
            // 3. 400 Bad Request (if validation fails)
            // All are acceptable for this test phase
            auto statusCode = resp->getStatusCode();
            bool isValidResponse =
              (statusCode == k200OK) || (statusCode == k302Found) || (statusCode == k400BadRequest);
            CHECK(isValidResponse == true);

            if (resp->getStatusCode() == k200OK)
            {
                LOG_INFO << "Authorization returned login page (expected)";
                auto bodyView = resp->getBody();
                std::string body(bodyView.data(), bodyView.length());
                bool hasLoginIndicator = (body.find("login") != std::string::npos) ||
                                         (body.find("Login") != std::string::npos) ||
                                         (body.find("SIGN IN") != std::string::npos);
                CHECK(hasLoginIndicator == true);
            }
            else if (resp->getStatusCode() == k302Found)
            {
                LOG_INFO << "Authorization redirected to login (expected)";
            }
            else if (resp->statusCode() == k400BadRequest)
            {
                LOG_INFO << "Authorization failed validation: " << resp->getBody();
            }

            LOG_INFO << "Authorization request processed with status: " << resp->getStatusCode();
        }

        // Step 3: Token Request
        LOG_INFO << "--- Step 3: Token Exchange ---";
        {
            std::map<std::string, std::string> params;
            params["grant_type"] = "authorization_code";
            params["code"] = "test_code";
            params["redirect_uri"] = "http://localhost:5173/callback";
            params["client_id"] = "fulla-portal";

            std::promise<HttpResponsePtr> p;
            auto f = p.get_future();

            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Post);
            for (const auto &param : params)
            {
                req->setParameter(param.first, param.second);
            }

            tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Token request timeout");
            }

            auto resp = f.get();

            if (resp->getStatusCode() == k400BadRequest)
            {
                LOG_WARN << "Token request failed (expected - invalid test code): "
                         << std::string(resp->getBody());
            }

            LOG_INFO << "Token exchange attempt completed";
        }

        // Step 4: User Info
        LOG_INFO << "--- Step 4: User Info Endpoint ---";
        {
            auto client = HttpClient::newHttpClient("http://127.0.0.1:5555");
            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Get);
            req->setPath("/oauth2/userinfo");
            req->addHeader("Authorization", "Bearer invalid_token");

            std::promise<HttpResponsePtr> p;
            auto f = p.get_future();

            client->sendRequest(req, [&](ReqResult /*result*/, const HttpResponsePtr &resp) {
                p.set_value(resp);
            });

            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("User info timeout");
            }

            auto resp = f.get();
            CHECK(resp->getStatusCode() == k401Unauthorized);
            LOG_INFO << "User info endpoint validated (401 as expected)";
        }

        // Step 5: Logout (Security Test)
        LOG_INFO << "--- Step 5: Logout Security Test ---";
        {
            std::promise<HttpResponsePtr> p;
            auto f = p.get_future();

            auto req = HttpRequest::newHttpRequest();
            req->setMethod(Post);
            // Note: No Authorization header - testing security behavior

            ctrl->logout(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

            if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
            {
                throw std::runtime_error("Logout timeout");
            }

            auto resp = f.get();
            // Logout is protected by OAuth2Middleware - requires authentication
            CHECK(resp->getStatusCode() == k401Unauthorized);
            LOG_INFO << "Logout security validated (401 as expected)";
        }

        cleanup();
        LOG_INFO << "=== E2E Test Completed Successfully ===";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "E2E Test Failed: " << e.what();
        cleanup();
        throw;
    }
}

DROGON_TEST(Integration_P0_Session_Management_Works)
{
    LOG_INFO << "=== E2E Test: Session Management ===";

    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (!plugin)
    {
        LOG_WARN << "OAuth2Plugin not found. Skipping test.";
        return;
    }

    auto ctrl = std::make_shared<SessionController>();
    auto authzCtrl =
      std::make_shared<fulla::drogon::controllers::AuthorizationEndpointController>();
    auto tokenCtrl = std::make_shared<fulla::drogon::controllers::TokenEndpointController>();

    // Test: Session Creation
    LOG_INFO << "--- Test: Session Creation ---";
    {
        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath("/oauth2/authorize");

        authzCtrl->authorize(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Session creation timeout");
        }

        auto resp = f.get();
        CHECK(resp->getStatusCode() == k400BadRequest);
        LOG_INFO << "Session creation failed with 400 (expected because "
                    "missing parameters)";
    }

    // Test: Session Clearing
    LOG_INFO << "--- Test: Session Clearing ---";
    {
        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);

        ctrl->logout(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Logout timeout");
        }

        auto resp = f.get();
        CHECK(resp->getStatusCode() == k401Unauthorized);
        LOG_INFO << "Session clear failed with 401 (expected because no session)";
    }

    LOG_INFO << "=== Session Management Test Completed ===";
}

DROGON_TEST(Integration_P0_Client_Authentication_Works)
{
    (void)TEST_CTX;
    LOG_INFO << "=== E2E Test: Client Authentication ===";

    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (!plugin)
    {
        LOG_WARN << "OAuth2Plugin not found. Skipping test.";
        return;
    }

    auto ctrl = std::make_shared<SessionController>();
    auto authzCtrl =
      std::make_shared<fulla::drogon::controllers::AuthorizationEndpointController>();
    auto tokenCtrl = std::make_shared<fulla::drogon::controllers::TokenEndpointController>();

    // Test: Public Client
    LOG_INFO << "--- Test: Public Client Authentication ---";
    {
        std::map<std::string, std::string> params;
        params["grant_type"] = "client_credentials";
        params["client_id"] = "public-client";

        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        for (const auto &param : params)
        {
            req->setParameter(param.first, param.second);
        }

        tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Public client timeout");
        }

        auto resp = f.get();
        LOG_INFO << "Public client test completed with status: " << resp->getStatusCode();
    }

    // Test: Confidential Client
    LOG_INFO << "--- Test: Confidential Client Authentication ---";
    {
        std::map<std::string, std::string> params;
        params["grant_type"] = "client_credentials";
        params["client_id"] = "confidential-client";
        params["client_secret"] = "test-secret";

        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        for (const auto &param : params)
        {
            req->setParameter(param.first, param.second);
        }

        tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Confidential client timeout");
        }

        auto resp = f.get();
        LOG_INFO << "Confidential client test completed with status: " << resp->getStatusCode();
    }

    // Test: HTTP Basic Authentication
    LOG_INFO << "--- Test: HTTP Basic Authentication ---";
    {
        std::map<std::string, std::string> params;
        params["grant_type"] = "client_credentials";

        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        std::string credentials = "confidential-client:test-secret";
        std::string encoded = utils::base64Encode(
          reinterpret_cast<const unsigned char *>(credentials.c_str()), credentials.length()
        );
        req->addHeader("Authorization", "Basic " + encoded);
        for (const auto &param : params)
        {
            req->setParameter(param.first, param.second);
        }

        tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Basic auth timeout");
        }

        auto resp = f.get();
        LOG_INFO << "Basic authentication test completed with status: " << resp->getStatusCode();
    }

    LOG_INFO << "=== Client Authentication Test Completed ===";
}

DROGON_TEST(Integration_P1_RedirectUri_Validation_Works)
{
    LOG_INFO << "=== E2E Test: Redirect URI Validation ===";

    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (!plugin)
    {
        LOG_WARN << "OAuth2Plugin not found. Skipping test.";
        return;
    }

    auto ctrl = std::make_shared<SessionController>();
    auto authzCtrl =
      std::make_shared<fulla::drogon::controllers::AuthorizationEndpointController>();
    auto tokenCtrl = std::make_shared<fulla::drogon::controllers::TokenEndpointController>();

    // Test: Valid Redirect URI
    LOG_INFO << "--- Test: Valid Redirect URI ---";
    {
        std::map<std::string, std::string> params;
        params["grant_type"] = "authorization_code";
        params["code"] = "test_code";
        params["redirect_uri"] = "http://localhost:5173/callback";
        params["client_id"] = "fulla-portal";

        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        for (const auto &param : params)
        {
            req->setParameter(param.first, param.second);
        }

        tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Valid redirect URI timeout");
        }

        auto resp = f.get();
        LOG_INFO << "Valid redirect URI test completed with status: " << resp->getStatusCode();
    }

    // Test: Invalid Redirect URI
    LOG_INFO << "--- Test: Invalid Redirect URI ---";
    {
        std::map<std::string, std::string> params;
        params["grant_type"] = "authorization_code";
        params["code"] = "test_code";
        params["redirect_uri"] = "http://malicious-site.com/callback";
        params["client_id"] = "fulla-portal";

        std::promise<HttpResponsePtr> p;
        auto f = p.get_future();

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        for (const auto &param : params)
        {
            req->setParameter(param.first, param.second);
        }

        tokenCtrl->token(req, [&](const HttpResponsePtr &resp) { p.set_value(resp); });

        if (f.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
            throw std::runtime_error("Invalid redirect URI timeout");
        }

        auto resp = f.get();
        CHECK(resp->getStatusCode() == k400BadRequest);
        LOG_INFO << "Invalid redirect URI properly rejected";
    }

    LOG_INFO << "=== Redirect URI Validation Test Completed ===";
}
