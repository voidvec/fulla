#pragma once

#include <drogon/drogon.h>
#include <fulla/drogon/error/ErrorResponder.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace fulla::drogon::http
{
using SharedResponse = std::shared_ptr<std::function<void(const ::drogon::HttpResponsePtr &)>>;

struct OnceResponse
{
    SharedResponse callback;
    std::shared_ptr<std::atomic<bool>> completed;
};

// HTTP completion only: an elapsed deadline does not prove that a database
// transaction rolled back. Late storage completion may still be audited,
// but it cannot produce another HTTP response or retain the business sink.
inline OnceResponse makeOnceResponse(
  const ::drogon::HttpRequestPtr &req,
  SharedResponse cb,
  double timeoutSeconds = 10.0
)
{
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto timer = std::make_shared<std::atomic<::trantor::TimerId>>(0);
    auto *loop = ::drogon::app().getLoop();
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    auto finish = [req, cb, completed, timer, loop, deadline](
                    const ::drogon::HttpResponsePtr &response, bool timedOut
                  ) {
        if (completed->exchange(true))
            return;
        const bool expired = timedOut || std::chrono::steady_clock::now() >= deadline;
        const auto id = timer->exchange(0);
        if (id)
            loop->invalidateTimer(id);
        auto deliver = std::move(*cb);
        if (!deliver)
            return;
        if (expired)
            ::fulla::common::error::ErrorResponder::respond(
              req, std::move(deliver), "DB_QUERY_ERROR", "operation completion deadline elapsed"
            );
        else
            deliver(response);
    };
    auto wrapped = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      [finish](const ::drogon::HttpResponsePtr &response) { finish(response, false); }
    );
    timer->store(loop->runAfter(timeoutSeconds, [finish]() { finish(nullptr, true); }));
    if (completed->load())
    {
        const auto id = timer->exchange(0);
        if (id)
            loop->invalidateTimer(id);
    }
    return {std::move(wrapped), std::move(completed)};
}
}  // namespace fulla::drogon::http
