#include "SecurityHeaders.h"
#include <drogon/drogon.h>
#include <string>

namespace bootstrap
{

void setupSecurityHeaders()
{
    drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
          resp->addHeader("X-Content-Type-Options", "nosniff");
          resp->addHeader("X-Frame-Options", "SAMEORIGIN");

          // Only apply CSP to HTML pages to avoid breaking API calls
          if (resp->getContentType() == drogon::CT_TEXT_HTML)
          {
              std::string path = req->path();
              if (path.find("/docs/") == 0)
              {
                  // Swagger UI needs relaxed CSP
                  resp->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; "
                    "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://unpkg.com; "
                    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com "
                    "https://unpkg.com; "
                    "font-src 'self' https://fonts.gstatic.com; "
                    "img-src 'self' data: https:; "
                    "connect-src 'self' https://unpkg.com; "
                    "frame-ancestors 'self';"
                  );
              }
              else if (path == "/login")
              {
                  // The login page is an OAuth BROKER: after the form POST
                  // the browser is 302-redirected to the client's registered
                  // redirect_uri, which can be ANY https origin (or a
                  // loopback origin in dev). Modern browsers enforce
                  // form-action across that redirect, so 'self' here breaks
                  // every browser-based RP login (found by the P1-5 nextjs
                  // demo e2e; json-mode interop drivers never see it).
                  // https: blocks plain-http exfiltration; the two loopback
                  // entries cover http dev RPs.
                  resp->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; "
                    "script-src 'self'; "
                    "style-src 'self' https://fonts.googleapis.com; "
                    "font-src 'self' https://fonts.gstatic.com; "
                    "img-src 'self' data:; "
                    "connect-src 'self'; "
                    "frame-ancestors 'none'; "
                    "base-uri 'self'; "
                    "form-action 'self' https: http://localhost:* http://127.0.0.1:*;"
                  );
              }
              else
              {
                  // Strict CSP for other fulla-hosted HTML
                  resp->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; "
                    "script-src 'self'; "
                    "style-src 'self' https://fonts.googleapis.com; "
                    "font-src 'self' https://fonts.gstatic.com; "
                    "img-src 'self' data:; "
                    "connect-src 'self'; "
                    "frame-ancestors 'none'; "
                    "base-uri 'self'; "
                    "form-action 'self';"
                  );
              }
          }

          // Only set HSTS header on HTTPS connections
          // Check X-Forwarded-Proto header for reverse proxy scenarios
          auto forwardedProto = req->getHeader("X-Forwarded-Proto");
          if (forwardedProto == "https")
          {
              resp->addHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
          }
      }
    );
}

}  // namespace bootstrap
