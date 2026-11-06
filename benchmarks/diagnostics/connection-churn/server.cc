// Minimal request-path probe for #303. No Fulla plugins, database or sessions.
// Shutdown/destructor behavior (#300) is outside this experiment.
#include <drogon/drogon.h>

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "Usage: %s PORT THREADS true|false(reuse_port)\n", argv[0]);
        return 2;
    }
    unsigned long port = 0;
    unsigned long threads = 0;
    try
    {
        size_t portEnd = 0;
        size_t threadsEnd = 0;
        port = std::stoul(argv[1], &portEnd);
        threads = std::stoul(argv[2], &threadsEnd);
        if (portEnd != std::string(argv[1]).size() ||
            threadsEnd != std::string(argv[2]).size() ||
            port < 1024 || port > 65535 || threads < 1 || threads > 128 ||
            (std::string(argv[3]) != "true" && std::string(argv[3]) != "false"))
            throw std::invalid_argument("out of range");
    }
    catch (const std::exception &)
    {
        std::fprintf(stderr, "Invalid arguments: PORT=1024..65535, THREADS=1..128, reuse=true|false\n");
        return 2;
    }

    Json::Value config;
    config["app"]["threads_num"] = static_cast<unsigned>(threads);
    config["app"]["reuse_port"] = std::string(argv[3]) == "true";
    config["listeners"][0]["address"] = "127.0.0.1";
    config["listeners"][0]["port"] = static_cast<unsigned>(port);
    std::atomic<unsigned long long> handled{0};
    drogon::app().loadConfigJson(config);
    drogon::app().registerHandler(
      "/ping",
      [&handled](const drogon::HttpRequestPtr &,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
          const auto count = handled.fetch_add(1, std::memory_order_relaxed) + 1;
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setBody("pong");
          response->addHeader("X-Diagnostic-Handled", std::to_string(count));
          cb(response);
      },
      {drogon::Get});
    drogon::app().run();
}
