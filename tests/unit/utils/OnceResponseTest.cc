#include "common/OnceResponse.h"
#include <drogon/drogon_test.h>
#include <future>
#include <thread>
#include <vector>

namespace
{
using fulla::server::detail::makeOnceResponse;
using fulla::server::detail::SharedResponse;

SharedResponse sink(
  const std::shared_ptr<std::atomic<int>> &calls,
  const std::shared_ptr<std::promise<int>> &result,
  const std::shared_ptr<int> &witness
)
{
    return std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      [calls, result, witness](const ::drogon::HttpResponsePtr &response) {
          (void)witness;
          if (calls->fetch_add(1) == 0)
              result->set_value(static_cast<int>(response->getStatusCode()));
      }
    );
}
}  // namespace

DROGON_TEST(Unit_P1_OnceResponse_ConcurrentCompletionReleasesBusinessCallback)
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto result = std::make_shared<std::promise<int>>();
    auto future = result->get_future();
    auto witness = std::make_shared<int>(1);
    std::weak_ptr<int> weak = witness;
    auto completion =
      makeOnceResponse(::drogon::HttpRequest::newHttpRequest(), sink(calls, result, witness));
    witness.reset();
    const auto response = ::drogon::HttpResponse::newHttpResponse();
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([completion, response]() { (*completion.callback)(response); });
    for (auto &thread : threads)
        thread.join();
    CHECK(future.get() == 200);
    CHECK(calls->load() == 1);
    CHECK(completion.completed->load());
    CHECK(weak.expired());
    (*completion.callback)(response);
    CHECK(calls->load() == 1);
}

DROGON_TEST(Unit_P1_OnceResponse_TimeoutAndLateReplyCompleteOnce)
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto result = std::make_shared<std::promise<int>>();
    auto future = result->get_future();
    auto witness = std::make_shared<int>(1);
    std::weak_ptr<int> weak = witness;
    auto completion =
      makeOnceResponse(::drogon::HttpRequest::newHttpRequest(), sink(calls, result, witness), 0.05);
    witness.reset();
    REQUIRE(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(future.get() == 500);
    (*completion.callback)(::drogon::HttpResponse::newHttpResponse());
    const auto releasedBy = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak.expired() && std::chrono::steady_clock::now() < releasedBy)
        std::this_thread::yield();
    CHECK(calls->load() == 1);
    CHECK(weak.expired());
}

DROGON_TEST(Unit_P1_OnceResponse_LateSuccessCannotBeatDelayedTimer)
{
    auto entered = std::make_shared<std::promise<void>>();
    auto blocked = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto resume = release->get_future().share();

    struct Unblock
    {
        std::shared_ptr<std::promise<void>> promise;

        ~Unblock()
        {
            try
            {
                promise->set_value();
            }
            catch (...)
            {
            }
        }
    } unblock{release};

    ::drogon::app().getLoop()->queueInLoop([entered, resume]() {
        entered->set_value();
        resume.wait();
    });
    REQUIRE(blocked.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto result = std::make_shared<std::promise<int>>();
    auto future = result->get_future();
    auto completion = makeOnceResponse(
      ::drogon::HttpRequest::newHttpRequest(), sink(calls, result, std::make_shared<int>(1)), 0.02
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    // The timer cannot run on the blocked loop. The DB-thread equivalent
    // success still has to reject against the actual steady deadline.
    (*completion.callback)(::drogon::HttpResponse::newHttpResponse());
    CHECK(future.get() == 500);
    release->set_value();
    auto drained = std::make_shared<std::promise<void>>();
    auto ready = drained->get_future();
    ::drogon::app().getLoop()->queueInLoop([drained]() { drained->set_value(); });
    CHECK(ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(calls->load() == 1);
}
