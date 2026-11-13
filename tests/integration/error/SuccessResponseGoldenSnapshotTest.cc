// Feature: error-code-message-standardization — Task 12.4 integration test.
//
// 成功响应体黄金快照回归测试
// (success-response-body-and-route-manifest golden-snapshot regression).
//
// Validates: Requirements 11.1, 11.2, 11.4
//   - 11.1: THE Backend SHALL 保持所有现有端点的 HTTP 路径与 HTTP 方法与迁移前基线
//           完全一致，不新增、删除或重命名既有路径与方法。
//   - 11.2: WHEN 某现有端点返回成功响应（HTTP 200..299），THE Backend SHALL 保持其响
//           应体的顶层 JSON 键集合、字段名、字段类型与嵌套层级与迁移前基线一致。
//   - 11.4: （间接）成功体快照固定了成功路径的结构，配合错误迁移测试保证错误迁移不会
//           波及成功响应结构。
//
// ---------------------------------------------------------------------------
// Approach (golden-snapshot, self-seeding, DB-independent)
// ---------------------------------------------------------------------------
// This feature migrates ERROR responses to a unified Error Envelope. Requirement
// 11 guards that this migration does NOT silently alter the public contract of
// SUCCESS responses or the route table. Two golden snapshots are committed under
// test/integration/error/golden/ and compared on every run:
//
//   1) route_manifest.txt — the sorted, de-duplicated set of every registered
//      route as "<METHOD> <path>". It is built from drogon::app().
//      getHandlersInfo() using ONLY column 0 (path) and column 1 (HttpMethod);
//      column 2 (the framework-internal handler name) is intentionally excluded
//      because it is volatile. Any added / removed / renamed path or method
//      changes this snapshot and fails the test (Requirement 11.1).
//
//   2) success_shapes.txt — the top-level "key:type" shape of representative 2xx
//      success bodies, captured by invoking the controller directly (no live
//      socket, no database). Only key NAMES and TYPES are recorded — never
//      values — so volatile fields (e.g. health timestamp) do not destabilise
//      the snapshot. Any change to a success body's key set / field name / field
//      type changes this snapshot and fails the test (Requirement 11.2).
//
// Self-seeding: on first run (or in a fresh checkout) a golden file that does not
// yet exist is created from the current snapshot and the test PASSES, recording
// the migration baseline. On every later run the current snapshot is compared to
// the committed golden and any divergence FAILS with a clear diff plus a hint to
// update the golden only if the change is intentional.
//
// DB independence: the success-body snapshot exercises GET /health/live, which
// the controller answers with { "status": "ok" } and NO database access, so it
// runs under any storage_type. GET /health is snapshotted only when it returns
// 200 (its top-level shape is then deterministic); GET /health/ready is NEVER
// snapshotted here because it depends on live DB + Redis.
// ---------------------------------------------------------------------------

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include "../../common/EnabledFeatureRoutes.h"

#include <fulla/drogon/controllers/HealthController.h>
using fulla::drogon::controllers::HealthController;

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace drogon;

namespace
{

// Absolute path to the committed golden directory, injected by the test
// CMakeLists via a compile definition (FULLA_GOLDEN_DIR) so the snapshot files
// can be located/seeded regardless of the test executable's working directory
// (CMake normalises the path with forward slashes, usable by std::ifstream /
// std::ofstream on all platforms).
#ifndef FULLA_GOLDEN_DIR
#define FULLA_GOLDEN_DIR ""
#endif

// Map a drogon HttpMethod to a stable, human-readable token for the manifest.
// Explicit per-enumerator switch so an upstream enum change surfaces as a build
// signal rather than a silently mislabelled route.
const char *methodName(HttpMethod method)
{
    switch (method)
    {
        case Get:
            return "GET";
        case Post:
            return "POST";
        case Head:
            return "HEAD";
        case Put:
            return "PUT";
        case Delete:
            return "DELETE";
        case Options:
            return "OPTIONS";
        case Patch:
            return "PATCH";
        case Invalid:
            return "INVALID";
        default:
            return "OTHER";
    }
}

// Classify a JSON value into a coarse, stable type token. isBool() MUST be
// checked before isIntegral() because jsoncpp treats booleanValue as integral.
std::string jsonTypeName(const Json::Value &value)
{
    if (value.isObject())
    {
        return "object";
    }
    if (value.isArray())
    {
        return "array";
    }
    if (value.isString())
    {
        return "string";
    }
    if (value.isBool())
    {
        return "bool";
    }
    if (value.isIntegral())
    {
        return "int";
    }
    if (value.isDouble())
    {
        return "double";
    }
    if (value.isNull())
    {
        return "null";
    }
    return "unknown";
}

// Strip any carriage returns so golden files use LF endings on every platform
// and comparisons are not perturbed by Git autocrlf / editor settings.
std::string stripCr(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c != '\r')
        {
            out.push_back(c);
        }
    }
    return out;
}

// Read a file as text (LF-normalised). Returns false if the file is absent.
bool readGolden(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = stripCr(ss.str());
    return true;
}

// Write a file as text with LF endings, creating parent directories as needed.
bool writeGolden(const std::string &path, const std::string &content)
{
    try
    {
        const std::filesystem::path p(path);
        if (p.has_parent_path())
        {
            std::filesystem::create_directories(p.parent_path());
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Failed to create golden directory for " << path << ": " << e.what();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << content;
    return out.good();
}

// Produce a readable diff of two newline-delimited snapshots: lines present only
// in the committed golden are prefixed "- ", lines only in the current snapshot
// "+ ". Used to make a snapshot regression actionable.
std::string diffSnapshots(const std::string &golden, const std::string &current)
{
    auto toSet = [](const std::string &s) {
        std::set<std::string> lines;
        std::istringstream ss(s);
        std::string line;
        while (std::getline(ss, line))
        {
            if (!line.empty())
            {
                lines.insert(line);
            }
        }
        return lines;
    };
    const std::set<std::string> g = toSet(golden);
    const std::set<std::string> c = toSet(current);

    std::ostringstream diff;
    for (const auto &line : g)
    {
        if (c.find(line) == c.end())
        {
            diff << "  - (only in golden)  " << line << "\n";
        }
    }
    for (const auto &line : c)
    {
        if (g.find(line) == g.end())
        {
            diff << "  + (only in current) " << line << "\n";
        }
    }
    return diff.str();
}

// Compare @p current against the committed golden file @p fileName. On first run
// (golden absent) the golden is seeded from @p current and the check PASSES; on
// later runs any divergence FAILS with a diff. Returns void; reports via TEST_CTX.
void compareOrSeed(
  std::shared_ptr<drogon::test::Case> TEST_CTX,
  const std::string &fileName,
  const std::string &current
)
{
    const std::string goldenDir = FULLA_GOLDEN_DIR;
    REQUIRE(!goldenDir.empty());  // Compile definition must be set by CMake.

    const std::string path = goldenDir + "/" + fileName;

    std::string golden;
    const bool exists = readGolden(path, golden);
    if (!exists)
    {
#if !defined(WITH_SOCIAL) || !defined(WITH_WEBAUTHN)
        if (fileName == "route_manifest.txt")
        {
            // A trimmed binary must never seed the full product baseline.
            LOG_ERROR << "Missing full-feature route golden: " << path;
            CHECK(exists);
            return;
        }
#endif
        // First run / fresh checkout: seed the baseline and pass.
        const bool wrote = writeGolden(path, current);
        if (!wrote)
        {
            LOG_ERROR << "Failed to seed golden file: " << path;
        }
        CHECK(wrote);
        LOG_WARN << "Seeded golden snapshot (baseline recorded): " << path << "\n" << current;
        return;
    }

    if (fileName == "route_manifest.txt")
        golden = fulla::test::features::expectedRoutes(golden);
    const std::string normalizedCurrent = stripCr(current);
    if (golden != normalizedCurrent)
    {
        LOG_ERROR << "Golden snapshot regression for " << path
                  << "\nThis means a previously-baselined endpoint contract changed."
                  << "\nDiff (golden vs current):\n"
                  << diffSnapshots(golden, normalizedCurrent)
                  << "If this change is INTENTIONAL, update the golden file by deleting it and "
                     "re-running the test to re-seed, then commit the new baseline.";
    }
    CHECK(golden == normalizedCurrent);
}

// Invoke a controller handler synchronously via a promise/future (mirrors the
// pattern used by test/e2e/oauth2_flows/OAuth2FlowE2ETest.cc) and return the
// captured response, or nullptr on timeout.
HttpResponsePtr callHandler(
  const std::function<void(const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&)>
    &handler,
  const HttpRequestPtr &req
)
{
    std::promise<HttpResponsePtr> p;
    auto f = p.get_future();
    handler(req, [&p](const HttpResponsePtr &resp) { p.set_value(resp); });
    if (f.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
    {
        return nullptr;
    }
    return f.get();
}

// Parse a response body as JSON. Returns false on parse failure.
bool parseJson(const HttpResponsePtr &resp, Json::Value &out)
{
    const std::string body(resp->getBody());
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(body.data(), body.data() + body.size(), &out, &errs);
}

// Render the top-level "key:type" shape of a JSON object as a sorted, comma-free
// multi-key line: e.g. "{database:string, status:string, timestamp:int}". Only
// key NAMES and TYPES are recorded — never values — so volatile fields do not
// destabilise the snapshot (Requirement 11.2).
std::string topLevelShape(const Json::Value &root)
{
    if (!root.isObject())
    {
        return "{<non-object:" + jsonTypeName(root) + ">}";
    }
    std::map<std::string, std::string> shape;  // ordered by key for stability
    for (const auto &name : root.getMemberNames())
    {
        shape[name] = jsonTypeName(root[name]);
    }
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto &kv : shape)
    {
        if (!first)
        {
            ss << ", ";
        }
        ss << kv.first << ":" << kv.second;
        first = false;
    }
    ss << "}";
    return ss.str();
}

}  // namespace

// ===========================================================================
// Requirement 11.1: route manifest golden snapshot. The set of registered
// (METHOD, path) pairs must not change across the error-handling migration.
// ===========================================================================
DROGON_TEST(Integration_P0_RouteManifest_GoldenSnapshot)
{
    const std::vector<HttpHandlerInfo> handlers = drogon::app().getHandlersInfo();
    REQUIRE(!handlers.empty());  // The booted app must have registered routes.

    // Build "<METHOD> <path>" from columns 0 (path) + 1 (method) ONLY; exclude
    // column 2 (volatile framework handler name). De-duplicate and sort so the
    // snapshot is a stable canonical set.
    std::set<std::string> lines;
    for (const auto &info : handlers)
    {
        const std::string &path = std::get<0>(info);
        const HttpMethod method = std::get<1>(info);
        lines.insert(std::string(methodName(method)) + " " + path);
    }

    std::ostringstream manifest;
    for (const auto &line : lines)  // std::set iterates in sorted order
    {
        manifest << line << "\n";
    }

    compareOrSeed(TEST_CTX, "route_manifest.txt", manifest.str());
}

// ===========================================================================
// Requirement 11.2: success-body shape golden snapshot. The top-level key set /
// field names / field types of representative 2xx bodies must not change across
// the migration. Captured via direct controller invocation (DB-independent).
// ===========================================================================
DROGON_TEST(Integration_P0_SuccessBodyShape_GoldenSnapshot)
{
    auto controller = std::make_shared<HealthController>();

    // Each recorded line: "<METHOD> <path> <httpStatus> <shape>". Only 2xx
    // bodies are snapshotted (Requirement 11.2 scopes success responses).
    std::vector<std::string> shapeLines;

    // --- GET /health/live: always 200, NO database. The canonical guaranteed
    //     deterministic success body { "status": "ok" }. -----------------------
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath("/health/live");

        const HttpResponsePtr resp = callHandler(
          [&controller](
            const HttpRequestPtr &r, std::function<void(const HttpResponsePtr &)> &&cb
          ) { controller->healthLive(r, std::move(cb)); },
          req
        );
        REQUIRE(resp != nullptr);

        const int status = static_cast<int>(resp->getStatusCode());
        CHECK(status >= 200);
        CHECK(status <= 299);

        Json::Value root;
        REQUIRE(parseJson(resp, root));
        shapeLines.push_back("GET /health/live 200 " + topLevelShape(root));
    }

    // --- GET /health: snapshot its top-level shape ONLY when it returns 200
    //     (then the shape is deterministic: status/service/timestamp plus the
    //     plugin-derived storage_type/database). When the plugin is unavailable
    //     it returns 503 with a different shape — guarded out to avoid a false
    //     regression. Values (incl. the volatile timestamp) are never recorded. -
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath("/health");

        const HttpResponsePtr resp = callHandler(
          [&controller](
            const HttpRequestPtr &r, std::function<void(const HttpResponsePtr &)> &&cb
          ) { controller->health(r, std::move(cb)); },
          req
        );
        REQUIRE(resp != nullptr);

        const int status = static_cast<int>(resp->getStatusCode());
        if (status == 200)
        {
            Json::Value root;
            REQUIRE(parseJson(resp, root));
            shapeLines.push_back("GET /health 200 " + topLevelShape(root));
        }
        else
        {
            LOG_WARN << "Skipping /health success-shape snapshot: status " << status
                     << " (plugin/DB unavailable in this harness).";
        }
    }

    // NOTE: GET /health/ready is intentionally NOT snapshotted here — its success
    // body depends on live DB + Redis connectivity.

    std::sort(shapeLines.begin(), shapeLines.end());
    std::ostringstream shapes;
    for (const auto &line : shapeLines)
    {
        shapes << line << "\n";
    }

    compareOrSeed(TEST_CTX, "success_shapes.txt", shapes.str());
}
