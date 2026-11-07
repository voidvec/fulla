// tests/common/SocialMockFixture.h
//
// Test fixture for HTTP integration tests that exercise the Social OAuth
// controllers (GitHub/Google/WeChat) WITHOUT real outbound network. The
// controllers are HttpController<T,false> (AutoCreation=false) process
// singletons with raw-pointer setXxxAuthService(...) setters. The setters are
// the only mock seam: a test constructs the REAL XxxAuthService backed by the
// shared FakeOAuthHttpClient (and FakeSocialAccountRepository for GitHub),
// then installs it on the controller singleton via
// drogon::DrClassMap::getSingleInstance<XxxController>()->setXxxService(...).
// The controller checks `if (service_)` first (GitHubController.cc:180,
// GoogleController.cc:168, WeChatController.cc:162), so the fake wins over
// the legacy inline-drogon::HttpClient fallback path.
//
// Wiring timing (verified): test_main.cc:374-391 runs bootstrap::wireIdentityServices()
// inside registerBeginningAdvice on the server thread BEFORE test::run() runs
// test cases on the main thread. In Postgres mode wireIdentityServices wires
// the REAL services (real DrogonOAuthHttpClient → real network); our setter,
// called later from a test case, overwrites them. In MEMORY mode
// wireIdentityServices early-returns (IdentityAssembly.cc:74-80) leaving the
// service pointers null → our fake is the ONLY wiring, so these tests run in
// every CI leg (Windows memory-mode included).
//
// Lifetime safety (review I-3): the fake services are stored in function-local
// `static shared_ptr`s -- process lifetime, never destroyed -- so the raw
// pointer handed to the controller stays valid for the whole test run. This
// avoids the use-after-free hazard that a per-case stack-local fake would
// create (DROGON_TEST case ordering is DrClassMap registration order, not
// source order, so "restore on scope exit" would be fragile). A test that
// wants a FRESH fake (clean response queues) calls the inject helper again,
// which constructs a new process-lifetime service.
//
// Thread-safety note (review I-2): the setter writes a non-atomic raw pointer
// from the test (main) thread while the server (loop) thread may read it. In
// practice this is benign-by-timing (the store happens-before the request),
// but it WILL flag under ThreadSanitizer. These mock-injection tests are
// therefore not suitable for -FULLA_SANITIZER=thread runs; run them under the
// default or ASan legs.

#pragma once

#include <fulla/drogon/controllers/AuthProvidersController.h>
#include <fulla/identity/testing/MemorySocialLoginStateStore.h>
#include <fulla/common/testing/FakeClock.h>
#include <fulla/common/testing/FakeCryptoProvider.h>
#include "HttpTestClient.h"
#include <fulla/drogon/controllers/GitHubController.h>
#include <fulla/drogon/controllers/GoogleController.h>
#include <fulla/drogon/controllers/WeChatController.h>
#include <fulla/drogon/controllers/UserSelfServiceController.h>
#include <fulla/identity/SocialAuthService.h>
#include <fulla/identity/SocialLinkService.h>
#include <fulla/identity/testing/FakeOAuthHttpClient.h>
#include <fulla/identity/testing/FakeSocialAccountRepository.h>
#include <fulla/identity/testing/MemorySocialLinkStateStore.h>

#include <drogon/drogon.h>  // DrClassMap

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fulla::test::social
{

using fulla::identity::testing::FakeOAuthHttpClient;
using fulla::identity::testing::FakeSocialAccountRepository;

// Install the same guard on every public social entry. Tests use the real
// HTTP initialization/issuance protocol below; they never mint login state
// directly or bypass Origin/cookie validation.
inline void injectLoginGuard()
{
    Json::Value config;
    config["frontend"]["url"] = "http://localhost:5173";
    config["cors"]["allow_origins"].append("http://localhost:5173");
    for (const auto *provider : {"github", "google", "wechat"})
    {
        config["external_auth"][provider]["client_id"] = "test-client-id";
        config["external_auth"][provider]["appid"] = "test-appid";
        config["external_auth"][provider]["client_secret"] = "test-client-secret";
        config["external_auth"][provider]["secret"] = "test-secret";
    }
    auto store = std::make_shared<fulla::identity::testing::MemorySocialLoginStateStore>(
      std::make_shared<fulla::common::testing::FakeClock>());
    auto states = std::make_shared<fulla::identity::SocialLoginStateService>(
      store, std::make_shared<fulla::common::testing::FakeCryptoProvider>(123));
    auto guard = std::make_shared<fulla::drogon::services::SocialLoginGuard>(
      states, config, std::vector<std::string>{"github", "google", "wechat"}, true);
    ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::AuthProvidersController>()
      ->setSocialLoginGuard(guard);
    ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>()
      ->setSocialLoginGuard(guard);
    ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GoogleController>()
      ->setSocialLoginGuard(guard);
    ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WeChatController>()
      ->setSocialLoginGuard(guard);
}

struct LoginBrowser
{
    std::string cookie;
    std::string state;
    ::drogon::HttpResponsePtr send(const std::string &path, const std::string &body = "",
      const std::string &type = "application/json", const std::string &origin = "http://localhost:5173")
    {
        auto request = ::drogon::HttpRequest::newHttpRequest();
        request->setMethod(::drogon::Post);
        request->setPath(path);
        request->setBody(body);
        request->setContentTypeString(type);
        if (!origin.empty()) request->addHeader("Origin", origin);
        if (!cookie.empty()) request->addHeader("Cookie", cookie);
        // One-time operations must never be retried after a transport failure.
        auto result = fulla::test::http::sharedTestHttpClient()->sendRequest(
          request, fulla::test::http::fullaTestHttpTimeout());
        return result.first == ::drogon::ReqResult::Ok ? result.second : nullptr;
    }
    bool begin(const std::string &provider)
    {
        const auto path = "/api/auth/providers/" + provider + "/authorize";
        auto response = send(path);
        if (!response || response->statusCode() != ::drogon::k204NoContent) return false;
        const auto binding = response->getCookie("fulla-social-binding-dev");
        if (binding.value().empty()) return false;
        cookie = "fulla-social-binding-dev=" + binding.value();
        response = send(path);
        if (!response || response->statusCode() != ::drogon::k200OK) return false;
        auto json = response->getJsonObject();
        if (!json) return false;
        state = (*json)["state"].asString();
        return state.size() == 64;
    }
};

inline ::drogon::HttpResponsePtr sendLoginForm(const std::string &path, const std::string &body)
{
    LoginBrowser browser;
    const auto end = path.find('/', 5);
    if (!browser.begin(path.substr(5, end - 5))) return nullptr;
    return browser.send(path, body + "&state=" + browser.state, "application/x-www-form-urlencoded");
}

inline ::drogon::HttpResponsePtr sendLoginJson(const std::string &path, Json::Value body)
{
    LoginBrowser browser;
    const auto end = path.find('/', 5);
    if (!browser.begin(path.substr(5, end - 5))) return nullptr;
    body["state"] = browser.state;
    Json::StreamWriterBuilder writer;
    return browser.send(path, Json::writeString(writer, body));
}

// ---------------------------------------------------------------------------
// Per-provider injection helpers. Each returns a shared_ptr to the FakeOAuthHttpClient
// the installed service is bound to, so the caller can seed response queues and
// inspect recorded calls. The installed service itself lives for the process
// (static shared_ptr) -- see the header comment on why process-lifetime storage
// is required.
// ---------------------------------------------------------------------------

// Install a GoogleAuthService backed by `http` onto the GoogleController
// singleton. Credentials are arbitrary (the fake does not validate them); the
// test seeds the fake's response queues.
//
// #70: the service gets a FakeSocialAccountRepository so login() runs the
// full account-linking four-state flow (auto-create on NoMapping). Return
// the repo so tests can pre-seed linked accounts or flip auto-create off
// via the returned service's setAutoCreate().
//
// Implementation note: the service is rebuilt fresh on every call (so each
// test case gets a clean FakeOAuthHttpClient with its own response queues),
// and stored in a process-lifetime `static vector<shared_ptr<...>>` so the
// raw pointer handed to the controller stays valid for the whole test run
// (DROGON_TEST case order is not source order; a stack-local or single
// `static auto svc` would either dangle or reuse a stale bound http object
// across cases -- both cause use-after-free / wrong-fake bugs).
struct GoogleFakeHandle
{
    std::shared_ptr<FakeOAuthHttpClient> http;
    std::shared_ptr<FakeSocialAccountRepository> accountRepo;
    std::shared_ptr<fulla::identity::GoogleAuthService> service;
};

inline GoogleFakeHandle injectGoogleFake()
{
    injectLoginGuard();
    GoogleFakeHandle h;
    h.http = std::make_shared<FakeOAuthHttpClient>();
    h.accountRepo = std::make_shared<FakeSocialAccountRepository>();
    h.service = std::make_shared<fulla::identity::GoogleAuthService>(
      h.http, "test-client-id", "test-client-secret", "https://example.test/cb");
    h.service->setAccountRepository(h.accountRepo);
    static std::vector<std::shared_ptr<fulla::identity::GoogleAuthService>> keepAlive;
    keepAlive.push_back(h.service);
    auto ctrl = ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GoogleController>();
    if (ctrl)
        ctrl->setGoogleAuthService(h.service.get());
    return h;
}

// Install a WeChatAuthService backed by `http` onto the WeChatController
// singleton. #70: same FakeSocialAccountRepository wiring as Google (see
// injectGoogleFake). Same process-lifetime keepAlive pattern.
struct WeChatFakeHandle
{
    std::shared_ptr<FakeOAuthHttpClient> http;
    std::shared_ptr<FakeSocialAccountRepository> accountRepo;
    std::shared_ptr<fulla::identity::WeChatAuthService> service;
};

inline WeChatFakeHandle injectWeChatFake()
{
    injectLoginGuard();
    WeChatFakeHandle h;
    h.http = std::make_shared<FakeOAuthHttpClient>();
    h.accountRepo = std::make_shared<FakeSocialAccountRepository>();
    h.service = std::make_shared<fulla::identity::WeChatAuthService>(
      h.http, "test-appid", "test-secret");
    h.service->setAccountRepository(h.accountRepo);
    static std::vector<std::shared_ptr<fulla::identity::WeChatAuthService>> keepAlive;
    keepAlive.push_back(h.service);
    auto ctrl = ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::WeChatController>();
    if (ctrl)
        ctrl->setWeChatAuthService(h.service.get());
    return h;
}

// Install a GitHubAuthService backed by `http` + `accountRepo` onto the
// GitHubController singleton. Returns the account-repo shared_ptr so the caller
// can pre-seed linked accounts or set failCreate. The http shared_ptr is
// accessible via the returned repo's... no -- return both via out-params.
//
// NOTE (supersedes the old "review B1" warning): issueTokensForUser USED to
// call drogon::app().getDbClient() directly, which under memory storage hit
// an uncatchable assert() -- so GitHub fakes had to return errors only.
// Token issuance was since re-routed through OAuth2Plugin::saveTokenPair
// (the storage abstraction; MemoryTokenRepository in memory mode), so the
// GitHub HAPPY PATH is now safe and testable in every storage mode -- see
// Integration_P0_GitHubLogin_FakeExchange_ReturnsTokens. Queueing successful
// token+userinfo exchanges is allowed; only the legacy fallback path still
// touches the DB directly.
struct GitHubFakeHandle
{
    std::shared_ptr<FakeOAuthHttpClient> http;
    std::shared_ptr<FakeSocialAccountRepository> accountRepo;
};

inline GitHubFakeHandle injectGitHubFake()
{
    injectLoginGuard();
    GitHubFakeHandle h;
    h.http = std::make_shared<FakeOAuthHttpClient>();
    h.accountRepo = std::make_shared<FakeSocialAccountRepository>();
    auto svc = std::make_shared<fulla::identity::GitHubAuthService>(
      h.http, h.accountRepo, "test-client-id", "test-client-secret");
    static std::vector<std::shared_ptr<fulla::identity::GitHubAuthService>> keepAlive;
    keepAlive.push_back(svc);
    auto ctrl = ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::GitHubController>();
    if (ctrl)
        ctrl->setGitHubAuthService(svc.get());
    return h;
}

// ---------------------------------------------------------------------------
// B2 social link/unlink: install a SocialLinkService (all three provider
// services sharing one FakeOAuthHttpClient + one FakeSocialAccountRepository)
// onto the UserSelfServiceController singleton. The controller resolves the
// acting user via Postgres (public_sub/numeric dispatch), then the service
// runs entirely against the fakes -- so a PG-backed test can drive the full
// link/list/unlink lifecycle without any provider network or mapping-row DB
// writes. Same process-lifetime keepAlive contract as the helpers above.
// ---------------------------------------------------------------------------

struct SocialLinkFakeHandle
{
    std::shared_ptr<FakeOAuthHttpClient> http;
    std::shared_ptr<FakeSocialAccountRepository> accountRepo;
    // #71: mint a one-time link state bound to (userId, provider).
    std::function<std::string(int32_t, const std::string &)> mintState;
};

inline SocialLinkFakeHandle injectSocialLinkFake()
{
    SocialLinkFakeHandle h;
    h.http = std::make_shared<FakeOAuthHttpClient>();
    h.accountRepo = std::make_shared<FakeSocialAccountRepository>();
    auto github = std::make_shared<fulla::identity::GitHubAuthService>(
      h.http, h.accountRepo, "test-client-id", "test-client-secret");
    auto google = std::make_shared<fulla::identity::GoogleAuthService>(
      h.http, "test-client-id", "test-client-secret", "https://example.test/cb");
    auto wechat =
      std::make_shared<fulla::identity::WeChatAuthService>(h.http, "test-appid", "test-secret");
    // #71: in-memory one-time link state so HTTP tests run the real
    // begin/consume flow (and can pre-mint states for direct link POSTs).
    auto stateStore = std::make_shared<fulla::identity::testing::MemorySocialLinkStateStore>();
    auto svc = std::make_shared<fulla::identity::SocialLinkService>(
      github, google, wechat, h.accountRepo, nullptr, nullptr, stateStore);
    // Keep every dependency alive for the process -- the service holds them,
    // and the controller holds only a raw pointer to the service.
    static std::vector<std::shared_ptr<void>> keepAlive;
    keepAlive.push_back(github);
    keepAlive.push_back(google);
    keepAlive.push_back(wechat);
    keepAlive.push_back(stateStore);
    keepAlive.push_back(svc);
    // Exposed for tests: mint a state for (user, provider) without going
    // through the authorize endpoint (the internal id is the admin seed's).
    h.mintState = [stateStore](int32_t userId, const std::string &provider) {
        std::optional<std::string> token;
        stateStore->issue(
          userId, provider,
          [&token](std::optional<std::string> t) { token = std::move(t); });
        return token.value_or("");
    };
    auto ctrl =
      ::drogon::DrClassMap::getSingleInstance<fulla::drogon::controllers::UserSelfServiceController>(
      );
    if (ctrl)
        ctrl->setSocialLinkService(svc.get());
    return h;
}

}  // namespace fulla::test::social
