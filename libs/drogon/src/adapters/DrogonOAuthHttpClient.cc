#include <fulla/drogon/adapters/DrogonOAuthHttpClient.h>

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>

namespace fulla::drogon::adapters
{

namespace
{

// Split a full URL into (origin, path+query) so it can be passed to
// drogon::HttpClient::newHttpClient(origin) + HttpRequest::setPath(path).
// url must include a scheme (http:// or https://), matching how every
// caller of this port (Google/WeChat/GitHub auth services) constructs
// its URLs.
std::pair<std::string, std::string> splitUrl(const std::string &url)
{
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return {url, "/"};
    size_t pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos)
        return {url, "/"};
    return {url.substr(0, pathStart), url.substr(pathStart)};
}

}  // namespace

namespace
{

// #57: HttpClient::newHttpClient / sendRequest can throw on degenerate URLs
// (e.g. an empty host from a stored "https://" URI). This port is invoked
// from inside async DB callbacks (backchannel logout dispatch); an exception
// escaping there reaches the Drogon event loop and terminates the process.
// Degrade to a transport-failure result instead -- callers treat it as a
// failed delivery and audit it.
::fulla::identity::OAuthHttpResult transportFailureResult()
{
    ::fulla::identity::OAuthHttpResult out;
    out.transportOk = false;
    return out;
}

}  // namespace

void DrogonOAuthHttpClient::postForm(
  const std::string &url,
  const std::vector<std::pair<std::string, std::string>> &params,
  ResultCallback &&cb
)
{
    auto sharedCb = std::make_shared<ResultCallback>(std::move(cb));
    try
    {
        auto [origin, path] = splitUrl(url);
        auto client = ::drogon::HttpClient::newHttpClient(origin);
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(path);
        // GitHub's token endpoint answers form-encoded unless the request
        // asks for JSON (the fallback path in GitHubController::exchangeCodeForToken
        // sends the same header); getJsonObject() below cannot parse the
        // form-encoded default. GitHub's REST API also rejects requests
        // without a User-Agent.
        req->addHeader("Accept", "application/json");
        req->addHeader("User-Agent", "fulla-server");
        for (const auto &[key, value] : params)
            req->setParameter(key, value);

        client->sendRequest(
          req,
          [sharedCb, client](::drogon::ReqResult result, const ::drogon::HttpResponsePtr &response) {
              fulla::identity::OAuthHttpResult out;
              out.transportOk = result == ::drogon::ReqResult::Ok && response != nullptr;
              if (response)
              {
                  out.statusCode = response->getStatusCode();
                  auto json = response->getJsonObject();
                  if (json)
                      out.body = *json;
              }
              (*sharedCb)(out);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "DrogonOAuthHttpClient::postForm: transport setup failed for " << url
                  << ": " << e.what();
        (*sharedCb)(transportFailureResult());
    }
    catch (...)
    {
        LOG_ERROR << "DrogonOAuthHttpClient::postForm: unknown transport failure for " << url;
        (*sharedCb)(transportFailureResult());
    }
}

void DrogonOAuthHttpClient::getWithBearerToken(
  const std::string &url,
  const std::string &bearerToken,
  ResultCallback &&cb
)
{
    auto sharedCb = std::make_shared<ResultCallback>(std::move(cb));
    try
    {
        auto [origin, path] = splitUrl(url);
        auto client = ::drogon::HttpClient::newHttpClient(origin);
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setPath(path);
        if (!bearerToken.empty())
            req->addHeader("Authorization", "Bearer " + bearerToken);
        req->addHeader("Accept", "application/json");
        req->addHeader("User-Agent", "fulla-server");

        client->sendRequest(
          req,
          [sharedCb, client](::drogon::ReqResult result, const ::drogon::HttpResponsePtr &response) {
              fulla::identity::OAuthHttpResult out;
              out.transportOk = result == ::drogon::ReqResult::Ok && response != nullptr;
              if (response)
              {
                  out.statusCode = response->getStatusCode();
                  auto json = response->getJsonObject();
                  if (json)
                      out.body = *json;
              }
              (*sharedCb)(out);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "DrogonOAuthHttpClient::getWithBearerToken: transport setup failed for "
                  << url << ": " << e.what();
        (*sharedCb)(transportFailureResult());
    }
    catch (...)
    {
        LOG_ERROR << "DrogonOAuthHttpClient::getWithBearerToken: unknown transport failure for "
                  << url;
        (*sharedCb)(transportFailureResult());
    }
}

}  // namespace fulla::drogon::adapters
