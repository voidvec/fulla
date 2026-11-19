#include "../src/controllers/PasswordLoginSessionState.h"

#include <drogon/drogon.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <future>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
namespace state = ::fulla::drogon::controllers::detail;

struct Capture
{
    std::promise<::drogon::SessionPtr> promise;
    std::function<void(::drogon::Session &)> action;
};

std::mutex captureMutex;
std::map<std::string, std::shared_ptr<Capture>> captures;
std::string baseUrl;

// Only this dedicated test executable registers the handler. Sessions are
// created by the framework through its public HTTP API, without private access.
void captureSession(
  const ::drogon::HttpRequestPtr &request,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    std::shared_ptr<Capture> capture;
    {
        std::lock_guard<std::mutex> lock(captureMutex);
        auto found = captures.find(request->getHeader("X-C1a-Capture"));
        if (found != captures.end())
        {
            capture = found->second;
            captures.erase(found);
        }
    }
    if (!capture)
    {
        auto response = ::drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(::drogon::k400BadRequest);
        callback(response);
        return;
    }
    auto response = ::drogon::HttpResponse::newHttpResponse();
    try
    {
        if (capture->action)
            capture->action(*request->session());
        capture->promise.set_value(request->session());
    }
    catch (...)
    {
        capture->promise.set_exception(std::current_exception());
        response->setStatusCode(::drogon::k500InternalServerError);
    }
    callback(response);
}

class SessionFixture
{
  public:
    SessionFixture()
        : client_(::drogon::HttpClient::newHttpClient(baseUrl, ::drogon::app().getLoop()))
    {
        session = observe();
        if (!session || cookie_.empty())
            throw std::runtime_error("Framework did not create a session/cookie");
    }

    ::drogon::SessionPtr observe(std::function<void(::drogon::Session &)> action = {})
    {
        auto capture = std::make_shared<Capture>();
        auto future = capture->promise.get_future();
        capture->action = std::move(action);
        const auto nonce = ::drogon::utils::getUuid();
        {
            std::lock_guard<std::mutex> lock(captureMutex);
            captures.emplace(nonce, capture);
        }
        auto request = ::drogon::HttpRequest::newHttpRequest();
        request->setMethod(::drogon::Get);
        request->setPath("/session");
        request->addHeader("X-C1a-Capture", nonce);
        if (!cookie_.empty())
            request->addHeader("Cookie", cookie_);
        const auto result = client_->sendRequest(request, 3.0);
        {
            std::lock_guard<std::mutex> lock(captureMutex);
            captures.erase(nonce);
        }
        if (
          result.first != ::drogon::ReqResult::Ok || !result.second ||
          result.second->getStatusCode() != ::drogon::k200OK ||
          future.wait_for(std::chrono::seconds(1)) != std::future_status::ready
        )
            throw std::runtime_error("Framework session capture failed");
        for (const auto &cookie : result.second->getCookies())
        {
            if (cookie.first == "JSESSIONID")
                cookie_ = cookie.first + "=" + cookie.second.value();
        }
        return future.get();
    }

    const std::string &cookie() const
    {
        return cookie_;
    }

    ::drogon::SessionPtr session;

  private:
    ::drogon::HttpClientPtr client_;
    std::string cookie_;
};

TEST(PasswordLoginSessionState, SubjectWritePreservesClaimsFlagsAndSessionId)
{
    SessionFixture fixture;
    const auto cookie = fixture.cookie();
    const auto observed = fixture.observe([](::drogon::Session &session) {
        session.insert("userId", std::string("old-internal"));
        session.insert("sub", std::string("old-public"));
        session.insert("auth_time", int64_t{73});
        session.insert("amr", std::string("pwd mfa"));
        session.insert("mfa_pending", true);
        session.insert("must_change_password", true);
        state::setPasswordLoginSubject(session, int32_t{42}, "public-A");
    });
    EXPECT_EQ(observed, fixture.session);
    const auto &session = *observed;
    EXPECT_EQ(session.getOptional<std::string>("userId"), "42");
    EXPECT_EQ(session.getOptional<std::string>("sub"), "public-A");
    EXPECT_EQ(session.getOptional<int64_t>("auth_time"), 73);
    EXPECT_EQ(session.getOptional<std::string>("amr"), "pwd mfa");
    EXPECT_EQ(session.getOptional<bool>("mfa_pending"), true);
    EXPECT_EQ(session.getOptional<bool>("must_change_password"), true);
    EXPECT_EQ(fixture.cookie(), cookie);
}

TEST(PasswordLoginSessionState, EmptySessionGetsTypedValuesAndCookieRotation)
{
    SessionFixture fixture;
    const auto cookie = fixture.cookie();
    const auto observed = fixture.observe([](::drogon::Session &session) {
        state::setPasswordLoginSubject(
          session, (std::numeric_limits<int32_t>::max)(), "public-not-internal"
        );
        state::setPasswordLoginClaimsAndRotate(session, int64_t{2147483799});
    });
    EXPECT_EQ(observed, fixture.session);
    auto &session = *observed;
    EXPECT_EQ(session.getOptional<std::string>("userId"), "2147483647");
    EXPECT_EQ(session.getOptional<std::string>("sub"), "public-not-internal");
    EXPECT_EQ(session.getOptional<int64_t>("auth_time"), int64_t{2147483799});
    EXPECT_EQ(session.getOptional<std::string>("amr"), "pwd");
    EXPECT_FALSE(session.find("mfa_pending"));
    EXPECT_FALSE(session.find("must_change_password"));
    EXPECT_NE(fixture.cookie(), cookie);
}

TEST(PasswordLoginSessionState, ReplacesMfaAccountAndPreservesAllUnownedKeys)
{
    SessionFixture fixture;
    const std::map<std::string, std::string> preserved{
      {"mfa_code_challenge", "old-S256-challenge"},
      {"mfa_code_challenge_method", "S256"},
      {"pending_consent_csrf_slots", "[{\"n\":\"first\",\"t\":17}]"},
      {"pending_org_context_slots", "[{\"s\":\"first\",\"o\":8,\"t\":17}]"},
      {"webauthn_auth_challenge", "independent-challenge"},
      {"unknown-sentinel", "keep-me"}
    };
    const auto cookie = fixture.cookie();
    const auto observed = fixture.observe([preserved](::drogon::Session &session) {
        session.insert("userId", std::string("7"));
        session.insert("sub", std::string("public-A"));
        session.insert("auth_time", int64_t{12});
        session.insert("amr", std::string("pwd mfa"));
        session.insert("mfa_pending", true);
        session.insert("must_change_password", true);
        for (const auto &entry : preserved)
            session.insert(entry.first, entry.second);
        state::setPasswordLoginSubject(session, int32_t{9}, "public-B");
        state::setPasswordLoginClaimsAndRotate(session, int64_t{3456789012});
    });
    EXPECT_EQ(observed, fixture.session);
    auto &session = *observed;
    EXPECT_EQ(session.getOptional<std::string>("userId"), "9");
    EXPECT_EQ(session.getOptional<std::string>("sub"), "public-B");
    EXPECT_EQ(session.getOptional<int64_t>("auth_time"), int64_t{3456789012});
    EXPECT_EQ(session.getOptional<std::string>("amr"), "pwd");
    EXPECT_FALSE(session.find("mfa_pending"));
    EXPECT_FALSE(session.find("must_change_password"));
    for (const auto &entry : preserved)
        EXPECT_EQ(session.getOptional<std::string>(entry.first), entry.second);
    size_t count = 0;
    session.modify([&count](const ::drogon::Session::SessionMap &values) {
        count = values.size();
    });
    EXPECT_EQ(count, preserved.size() + 4);
    EXPECT_NE(fixture.cookie(), cookie);
}

TEST(PasswordLoginSessionState, SubjectKeepsFullSignedInternalIdAndPublicSubjectDistinct)
{
    SessionFixture fixture;
    const auto cookie = fixture.cookie();
    const auto observed = fixture.observe([](::drogon::Session &session) {
        state::setPasswordLoginSubject(session, (std::numeric_limits<int32_t>::min)(), "public-A");
    });
    EXPECT_EQ(observed->getOptional<std::string>("userId"), "-2147483648");
    EXPECT_EQ(observed->getOptional<std::string>("sub"), "public-A");
    EXPECT_FALSE(observed->find("auth_time"));
    EXPECT_FALSE(observed->find("amr"));
    EXPECT_EQ(fixture.cookie(), cookie);
}

TEST(PasswordLoginSessionState, ClaimsKeepSubjectsAndFullSignedTimeRange)
{
    SessionFixture fixture;
    for (const auto time :
         {int64_t{0}, (std::numeric_limits<int64_t>::min)(), (std::numeric_limits<int64_t>::max)()})
    {
        const auto cookie = fixture.cookie();
        const auto observed = fixture.observe([time](::drogon::Session &session) {
            session.insert("userId", std::string("same-internal"));
            session.insert("sub", std::string("same-public"));
            session.erase("amr");
            session.insert("amr", std::string("pwd mfa"));
            session.insert("mfa_pending", true);
            session.insert("must_change_password", true);
            state::setPasswordLoginClaimsAndRotate(session, time);
        });
        EXPECT_EQ(observed, fixture.session);
        auto &session = *observed;
        EXPECT_EQ(session.getOptional<int64_t>("auth_time"), time);
        EXPECT_EQ(session.getOptional<std::string>("amr"), "pwd");
        EXPECT_EQ(session.getOptional<std::string>("userId"), "same-internal");
        EXPECT_EQ(session.getOptional<std::string>("sub"), "same-public");
        EXPECT_FALSE(session.find("mfa_pending"));
        EXPECT_FALSE(session.find("must_change_password"));
        EXPECT_NE(fixture.cookie(), cookie);
    }
}
}  // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    auto ready = std::make_shared<std::promise<uint16_t>>();
    auto future = ready->get_future();
    auto serverFailed = std::make_shared<std::atomic<bool>>(false);
    std::thread server;
    int result = 1;
    try
    {
        ::drogon::app()
          .setLogLevel(trantor::Logger::kWarn)
          .setThreadNum(1)
          .enableSession()
          .addListener("127.0.0.1", 0)
          .registerHandler("/session", &captureSession, {::drogon::Get})
          .registerBeginningAdvice([ready] {
              ::drogon::app().getLoop()->queueInLoop([ready] {
                  const auto listeners = ::drogon::app().getListeners();
                  ready->set_value(listeners.empty() ? 0 : listeners.front().toPort());
              });
          });
        server = std::thread([ready, serverFailed] {
            try
            {
                ::drogon::app().run();
            }
            catch (...)
            {
                serverFailed->store(true);
                try
                {
                    ready->set_exception(std::current_exception());
                }
                catch (const std::future_error &)
                {
                }
            }
        });
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            throw std::runtime_error("Session fixture startup deadline exceeded");
        const auto port = future.get();
        if (port == 0)
            throw std::runtime_error("Session fixture did not bind an ephemeral port");
        baseUrl = "http://127.0.0.1:" + std::to_string(port);
        result = RUN_ALL_TESTS();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Session fixture failed: " << error.what() << '\n';
    }
    ::drogon::app().quit();
    if (server.joinable())
        server.join();
    return serverFailed->load() ? 1 : result;
}
