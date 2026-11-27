#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <drogon/plugins/PromExporter.h>
#include <drogon/utils/monitoring/Collector.h>
#include <fulla/drogon/adapters/DrogonMetrics.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include "HttpTestClient.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// Regression for the S07 capacity harness cache-proof gate: the driver
// scrapes fulla_cache_total{repo="token",outcome=...} from /metrics to prove
// the introspection cache path ran. The IMetrics adapter used to be
// log-only, so the counters documented in
// docs/domains/token-refresh-and-revocation.md never reached PromExporter
// and the gate could never pass (row 013-introspect_reuse reported
// cache_path_not_proven despite 100% flow success).

DROGON_TEST(Integration_P2_Plugin_DrogonMetrics_PromExporterExport)
{
    auto plugin = ::drogon::app().getPlugin<::drogon::plugin::PromExporter>();
    REQUIRE(plugin != nullptr);

    const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string counterName = "fulla_test_cache_total_" + suffix;
    const std::string gaugeName = "fulla_test_active_" + suffix;

    fulla::drogon::adapters::DrogonMetrics metrics;
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});
    metrics.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "miss"}});

    // A second adapter instance must reuse the registered collector rather
    // than trip PromExporter's duplicate-registration guard.
    fulla::drogon::adapters::DrogonMetrics second;
    second.incrementCounter(
      counterName, {{"repo", "token"}, {"outcome", "hit"}});

    std::shared_ptr<::drogon::monitoring::CollectorBase> collector;
    try
    {
        collector = plugin->getCollector(counterName);
    }
    catch (const std::exception &e)
    {
        FAIL(std::string("counter collector not exported: ") + e.what());
        return;
    }
    REQUIRE(collector != nullptr);
    double hit = -1;
    double miss = -1;
    for (const auto &group : collector->collect())
    {
        std::string outcome;
        for (const auto &kv : group.metric->labels())
        {
            if (kv.first == "outcome")
                outcome = kv.second;
        }
        double sum = 0;
        for (const auto &sample : group.samples)
            sum += sample.value;
        if (outcome == "hit")
            hit = sum;
        if (outcome == "miss")
            miss = sum;
    }
    CHECK(hit == 3.0);
    CHECK(miss == 1.0);

    metrics.setGauge(gaugeName, {{"kind", "active"}}, 42.5);
    metrics.setGauge(gaugeName, {{"kind", "active"}}, 7.0);
    std::shared_ptr<::drogon::monitoring::CollectorBase> gaugeCollector;
    try
    {
        gaugeCollector = plugin->getCollector(gaugeName);
    }
    catch (const std::exception &e)
    {
        FAIL(std::string("gauge collector not exported: ") + e.what());
        return;
    }
    REQUIRE(gaugeCollector != nullptr);
    double gauge = -1;
    for (const auto &group : gaugeCollector->collect())
    {
        for (const auto &sample : group.samples)
            gauge = sample.value;
    }
    CHECK(gauge == 7.0);

    // End-to-end: the Prometheus exposition the S07 driver scrapes must
    // carry the counter with the labels the gate filters on.
    auto client = ::drogon::HttpClient::newHttpClient(
      "http://127.0.0.1:5555", ::drogon::app().getLoop());
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setPath("/metrics");
    auto [result, resp] = client->sendRequest(req, 10);
    REQUIRE(result == ::drogon::ReqResult::Ok);
    REQUIRE(resp != nullptr);
    REQUIRE(resp->statusCode() == ::drogon::k200OK);
    const std::string body(resp->body());
    CHECK(body.find(counterName + "{") != std::string::npos);
    CHECK(body.find("repo=\"token\"") != std::string::npos);
    CHECK(body.find("outcome=\"hit\"") != std::string::npos);
}

namespace
{
using EndpointStatusKey = std::pair<std::string, std::string>;
using OAuth2RequestMap = std::map<EndpointStatusKey, double>;

OAuth2RequestMap readCollectorOAuth2Requests(
  ::drogon::plugin::PromExporter &exporter
)
{
    OAuth2RequestMap counts;
    try
    {
        auto collector = exporter.getCollector("oauth2_requests_total");
        if (!collector)
            return counts;
        for (const auto &group : collector->collect())
        {
            std::string endpoint;
            std::string status;
            for (const auto &kv : group.metric->labels())
            {
                if (kv.first == "endpoint")
                    endpoint = kv.second;
                else if (kv.first == "status")
                    status = kv.second;
            }
            double sum = 0.0;
            for (const auto &sample : group.samples)
                sum += sample.value;
            counts[{endpoint, status}] += sum;
        }
    }
    catch (...)
    {
    }
    return counts;
}

bool extractLabelValue(
  const std::string &line,
  const std::string &label,
  std::string &out
)
{
    const std::string prefix = label + "=\"";
    const auto pos = line.find(prefix);
    if (pos == std::string::npos)
        return false;
    const auto start = pos + prefix.size();
    const auto end = line.find('"', start);
    if (end == std::string::npos)
        return false;
    out = line.substr(start, end - start);
    return true;
}

bool parseScrapedOAuth2Requests(
  const std::string &body,
  OAuth2RequestMap &out,
  bool &allHaveRequiredLabels
)
{
    out.clear();
    allHaveRequiredLabels = true;
    bool foundAny = false;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.rfind("oauth2_requests_total{", 0) != 0)
            continue;
        foundAny = true;
        std::string endpoint;
        std::string status;
        if (!extractLabelValue(line, "endpoint", endpoint) ||
            !extractLabelValue(line, "status", status))
        {
            allHaveRequiredLabels = false;
            continue;
        }
        const auto close = line.find('}');
        if (close == std::string::npos)
        {
            allHaveRequiredLabels = false;
            continue;
        }
        std::istringstream valStream(line.substr(close + 1));
        double val = 0.0;
        if (valStream >> val)
            out[{endpoint, status}] += val;
    }
    return foundAny;
}
}  // namespace

DROGON_TEST(Integration_P2_Plugin_OAuth2RequestsTotal_ScrapeCountsByStatus)
{
    REQUIRE(fulla::test::http::serverReachable());

    auto plugin = ::drogon::app().getPlugin<::drogon::plugin::PromExporter>();
    REQUIRE(plugin != nullptr);

    const OAuth2RequestMap before = readCollectorOAuth2Requests(*plugin);

    // 1. Failing /oauth2/authorize (missing state -> 400).
    {
        auto resp = fulla::test::http::sendGet(
          "/oauth2/authorize?response_type=code&client_id=fulla-portal"
          "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        );
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == ::drogon::k400BadRequest);
    }

    // 2. Failing /oauth2/token (unsupported grant_type -> 400).
    {
        auto resp = fulla::test::http::sendPostForm(
          "/oauth2/token",
          "grant_type=unsupported_grant_type_for_metrics_test"
        );
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == ::drogon::k400BadRequest);
    }

    const bool hasDb = fulla::test::http::postgresAvailable();
    if (hasDb)
    {
        // 3. Successful /oauth2/token (200) + /oauth2/consent?json=true (authorize 200).
        auto token = fulla::test::http::loginAsAdmin();
        REQUIRE(token.has_value());

        const std::string verifier = "F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po";
        const std::string challenge =
          ::fulla::drogon::utils::computeCodeChallenge(verifier, "S256");
        auto loginResp = fulla::test::http::sendPostForm(
          "/oauth2/login?json=true",
          "username=admin&password=admin&client_id=fulla-admin-console"
          "&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
          "&scope=openid&state=metrics_st_01&code_challenge=" +
            challenge + "&code_challenge_method=S256"
        );
        REQUIRE(loginResp != nullptr);
        REQUIRE(loginResp->getStatusCode() == ::drogon::k200OK);

        std::string cookie;
        for (const auto &entry : loginResp->getCookies())
        {
            if (!cookie.empty())
                cookie += "; ";
            cookie += entry.first + "=" + entry.second.value();
        }
        REQUIRE(!cookie.empty());

        auto client = ::drogon::HttpClient::newHttpClient(
          "http://127.0.0.1:5555", ::drogon::app().getLoop());
        auto authReq = ::drogon::HttpRequest::newHttpRequest();
        authReq->setMethod(::drogon::Get);
        authReq->setPath("/oauth2/authorize");
        authReq->setParameter("response_type", "code");
        authReq->setParameter("client_id", "fulla-admin-console");
        authReq->setParameter("redirect_uri", "http://127.0.0.1:5174/admin/callback");
        authReq->setParameter("scope", "openid");
        authReq->setParameter("state", "metrics_st_01");
        authReq->setParameter("prompt", "consent");
        authReq->setParameter("code_challenge", challenge);
        authReq->setParameter("code_challenge_method", "S256");
        authReq->addHeader("Cookie", cookie);
        auto [authRes, authResp] = client->sendRequest(authReq, 10.0);
        REQUIRE(authRes == ::drogon::ReqResult::Ok);
        REQUIRE(authResp != nullptr);
        REQUIRE(authResp->getStatusCode() == ::drogon::k302Found);
        for (const auto &entry : authResp->getCookies())
        {
            const std::string prefix = entry.first + "=";
            if (cookie.find(prefix) == std::string::npos)
                cookie += "; " + prefix + entry.second.value();
        }

        const std::string location = authResp->getHeader("Location");
        const auto csrfPos = location.find("consent_csrf=");
        const auto uidPos = location.find("user_id=");
        REQUIRE(csrfPos != std::string::npos);
        REQUIRE(uidPos != std::string::npos);
        const auto csrfEnd = location.find('&', csrfPos);
        const std::string csrf = location.substr(
          csrfPos + 13,
          (csrfEnd == std::string::npos ? location.size() : csrfEnd) - csrfPos - 13);
        const auto uidEnd = location.find('&', uidPos);
        const std::string userId = location.substr(
          uidPos + 8,
          (uidEnd == std::string::npos ? location.size() : uidEnd) - uidPos - 8);

        auto consentReq = ::drogon::HttpRequest::newHttpRequest();
        consentReq->setMethod(::drogon::Post);
        consentReq->setPath("/oauth2/consent?json=true");
        consentReq->setContentTypeCode(::drogon::CT_APPLICATION_X_FORM);
        consentReq->addHeader("Cookie", cookie);
        consentReq->setBody(
          "client_id=fulla-admin-console&user_id=" + userId +
          "&scope=openid&redirect_uri=http%3A%2F%2F127.0.0.1%3A5174%2Fadmin%2Fcallback"
          "&state=metrics_st_01&code_challenge=" +
          challenge + "&code_challenge_method=S256&consent_csrf=" + csrf +
          "&action=approve"
        );
        auto [cRes, cResp] = client->sendRequest(consentReq, 10.0);
        REQUIRE(cRes == ::drogon::ReqResult::Ok);
        REQUIRE(cResp != nullptr);
        CHECK(cResp->getStatusCode() == ::drogon::k200OK);
    }

    auto valueOf = [](const OAuth2RequestMap &m, const std::string &ep, const std::string &st) {
        auto it = m.find({ep, st});
        return it == m.end() ? 0.0 : it->second;
    };

    // PromExporter caches /metrics HTTP responses for 5s; poll within one 8s
    // deadline until the fresh snapshot reflects the emitted counts.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto scrapeClient = ::drogon::HttpClient::newHttpClient(
      "http://127.0.0.1:5555", ::drogon::app().getLoop());
    OAuth2RequestMap after;
    bool allLabelsOk = false;
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const double remaining =
          std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0)
            break;
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setPath("/metrics");
        auto [res, resp] = scrapeClient->sendRequest(req, (std::min)(remaining, 3.0));
        REQUIRE(res == ::drogon::ReqResult::Ok);
        REQUIRE(resp != nullptr);
        REQUIRE(resp->getStatusCode() == ::drogon::k200OK);
        const std::string body(resp->getBody());
        if (parseScrapedOAuth2Requests(body, after, allLabelsOk))
        {
            const bool failReady =
              (valueOf(after, "authorize", "400") - valueOf(before, "authorize", "400") >= 1.0) &&
              (valueOf(after, "token", "400") - valueOf(before, "token", "400") >= 1.0);
            const bool okReady =
              !hasDb ||
              ((valueOf(after, "authorize", "200") - valueOf(before, "authorize", "200") >= 1.0) &&
               (valueOf(after, "token", "200") - valueOf(before, "token", "200") >= 1.0));
            if (failReady && okReady)
            {
                ready = true;
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        std::this_thread::sleep_until((std::min)(deadline, now + std::chrono::milliseconds(50)));
    }

    CHECK(ready);
    CHECK(allLabelsOk);
    CHECK((valueOf(after, "authorize", "400") - valueOf(before, "authorize", "400")) == 1.0);
    CHECK((valueOf(after, "token", "400") - valueOf(before, "token", "400")) == 1.0);
    if (hasDb)
    {
        CHECK((valueOf(after, "authorize", "200") - valueOf(before, "authorize", "200")) == 1.0);
        CHECK((valueOf(after, "token", "200") - valueOf(before, "token", "200")) == 1.0);
    }
}