// examples/full-stack-host (Task 28a, fulla-sdk-refactor design.md §1.1/§6):
// the FULL-STACK SDK smoke. It boots a real Drogon HTTP server assembled
// entirely from the fulla product stack consumed via find_package
// (fulla::drogon + its transitive closure), reuses the product's
// controllers / OAuth2Plugin / views, and drives the authorization-code flow
// over HTTP to assert the three acceptance points of Task 28a:
//
//   1. Route registration    -- GET /health returns 200 (HealthController and
//                                the other 14 AutoCreation=false controllers
//                                were registered by bootstrap::registerAllControllers()).
//   2. Config-driven plugin  -- GET /.well-known/openid-configuration returns
//      instantiation            200 JSON with an "issuer": the DiscoveryController
//                                answer is produced by the OAuth2Plugin that
//                                drogon constructed by reflecting the "plugins"
//                                block of the memory config (storage_type=memory).
//   3. View rendering        -- GET /login renders apps/server/views/login.csp
//                                (compiled into this binary via drogon_create_views);
//                                the body carries the template's <title>.
//
// Plus an authorization-code entry check: GET /oauth2/authorize for the
// config-seeded fulla-portal, while unauthenticated, redirects to the login
// screen -- proving the client seeded from config is accepted by the flow.
//
// Storage is memory-backed (config's storage_type=memory), so the smoke is
// hermetic: no PostgreSQL, no Redis. wireIdentityServices() is safe with the
// empty db_clients block (it logs a warning and returns; see IdentityAssembly.h).
//
// Exit code: 0 = every check passed, 1 = any check failed. std::_Exit is used
// on the happy path to skip Drogon's teardown (same rationale as
// tests/test_main.cc -- avoids a shutdown-order SegFault after a passing run).

#include <drogon/drogon.h>

#include "bootstrap/ControllerRegistration.h"
#include "bootstrap/IdentityAssembly.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

namespace
{

std::atomic<int> g_failures{0};

void check(bool cond, const char *msg)
{
    if (cond)
    {
        std::fprintf(stderr, "[ ok ] %s\n", msg);
    }
    else
    {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        ++g_failures;
    }
    std::fflush(stderr);
}

}  // namespace

int main()
{
    using namespace drogon;

    // 1. Locate the memory smoke config (copied next to the exe by CMake).
    std::string configPath = "./config.json";
    if (!std::filesystem::exists(configPath))
        configPath = "../config.json";
    if (!std::filesystem::exists(configPath))
    {
        std::fprintf(stderr, "[FAIL] config.json not found next to executable\n");
        return 1;
    }
    app().loadConfigFile(configPath);

    // 2. Product-level assembly, identical to apps/server/src/main.cc but
    // consumed here purely from the SDK: register every AutoCreation=false
    // controller before run().
    bootstrap::registerAllControllers();

    // 3. Start the event loop on a background thread; signal readiness from a
    // beginning-advice callback (the point at which the OAuth2Plugin has been
    // constructed from config and dependencies can be wired) -- same handshake
    // as tests/test_main.cc.
    std::promise<void> ready;
    std::future<void> readyFuture = ready.get_future();
    std::atomic<bool> signaled{false};

    std::thread appThread([&]() {
        try
        {
            app().registerBeginningAdvice([&]() {
                bootstrap::wireControllerPluginDependencies();
                bootstrap::wireIdentityServices();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                bool expected = false;
                if (signaled.compare_exchange_strong(expected, true))
                    ready.set_value();
            });
            app().run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[FAIL] app().run() threw: %s\n", e.what());
            bool expected = false;
            if (signaled.compare_exchange_strong(expected, true))
                ready.set_value();
        }
    });

    if (readyFuture.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
    {
        std::fprintf(stderr, "[FAIL] server did not start within 60s\n");
        std::_Exit(1);
    }
    readyFuture.get();
    // Small prewarm so the listener is fully accepting before the first request.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 4. Drive the flow over real HTTP against the in-process server.
    const std::string base = "http://127.0.0.1:6789";
    auto client = HttpClient::newHttpClient(base);

    auto httpGet = [&](const std::string &path) {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath(path);
        return client->sendRequest(req, 15.0);
    };

    // (1) Route registration: /health -> 200.
    {
        auto [result, resp] = httpGet("/health");
        check(result == ReqResult::Ok && resp != nullptr, "GET /health reachable");
        if (resp)
            check(resp->statusCode() == k200OK, "GET /health -> 200 (controllers registered)");
    }

    // (2) Config-driven plugin instantiation: discovery reflects the
    // OAuth2Plugin built from the memory config block.
    {
        auto [result, resp] = httpGet("/.well-known/openid-configuration");
        check(result == ReqResult::Ok && resp != nullptr, "GET discovery reachable");
        if (resp)
        {
            check(
              resp->statusCode() == k200OK,
              "discovery -> 200 (OAuth2Plugin instantiated from config)"
            );
            auto json = resp->getJsonObject();
            check(
              json && json->isMember("issuer"), "discovery JSON carries an issuer (plugin wired)"
            );
        }
    }

    // (3) View rendering: /login renders apps/server/views/login.csp.
    {
        auto [result, resp] = httpGet(
          "/login?client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback"
          "&response_type=code&scope=openid"
        );
        check(result == ReqResult::Ok && resp != nullptr, "GET /login reachable");
        if (resp)
        {
            check(resp->statusCode() == k200OK, "GET /login -> 200");
            const std::string body(resp->getBody());
            check(
              body.find("Sign In - OAuth2 Platform") != std::string::npos,
              "/login rendered the login.csp view"
            );
        }
    }

    // (4) Authorization-code entry: unauthenticated /oauth2/authorize for the
    // config-seeded fulla-portal redirects to the login screen.
    // F-014 (RFC 8252 §7.3): redirect_uri uses a loopback IP literal
    // (127.0.0.1) which is allowed over plain http. F-011 (RFC 9700 §2.1.1):
    // PKCE is now required by default for the authorization_code grant, so the
    // authorize request carries a code_challenge (S256 of a fixed verifier).
    {
        auto [result, resp] = httpGet(
          "/oauth2/authorize?client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback"
          "&response_type=code&scope=openid&state=smoke-state-123"
          "&code_challenge=smoke-challenge-fixed-43chars-long-padding__&code_challenge_method=S256"
        );
        check(result == ReqResult::Ok && resp != nullptr, "GET /oauth2/authorize reachable");
        if (resp)
        {
            const int code = resp->statusCode();
            check(
              code == k302Found || code == k200OK,
              "/oauth2/authorize entered the auth-code flow (redirect/login)"
            );
        }
    }

    if (g_failures.load() == 0)
    {
        std::printf(
          "[+] full-stack SDK smoke PASSED: product stack consumed via find_package, "
          "routes registered + discovery served by config-driven OAuth2Plugin + "
          "login.csp view rendered + auth-code flow entered over HTTP.\n"
        );
        std::_Exit(0);
    }

    std::fprintf(stderr, "[x] full-stack SDK smoke FAILED (%d check(s))\n", g_failures.load());
    std::_Exit(1);
}
