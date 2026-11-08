#include <fulla/identity/SocialLoginStateService.h>
#include <fulla/identity/testing/MemorySocialLoginStateStore.h>
#include <fulla/common/testing/FakeClock.h>
#include <fulla/common/testing/FakeCryptoProvider.h>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace fulla::identity;
namespace
{
using Status = SocialLoginStateStatus;
struct Fixture
{
    std::shared_ptr<fulla::common::testing::FakeClock> clock =
      std::make_shared<fulla::common::testing::FakeClock>();
    std::shared_ptr<fulla::common::testing::FakeCryptoProvider> crypto =
      std::make_shared<fulla::common::testing::FakeCryptoProvider>(42);
    std::shared_ptr<fulla::identity::testing::MemorySocialLoginStateStore> store =
      std::make_shared<fulla::identity::testing::MemorySocialLoginStateStore>(clock);
    std::shared_ptr<SocialLoginStateService> service =
      std::make_shared<SocialLoginStateService>(store, crypto);
    SocialLoginContext context{"github", "client", "https://portal.test/callback/github",
                               std::string(64, 'a'), "login"};
    std::string issue()
    {
        std::string state;
        service->issue(context, [&](Status status, const std::string &value) {
            EXPECT_EQ(status, Status::Ok);
            state = value;
        });
        return state;
    }
    Status consume(const std::string &state, const SocialLoginContext &expected)
    {
        Status result = Status::Unavailable;
        service->consume(state, expected, [&](Status status) { result = status; });
        return result;
    }
};
class BrokenCrypto : public fulla::common::testing::FakeCryptoProvider
{
  public:
    bool secureRandomBytes(unsigned char *, size_t) override { return false; }
};
}

TEST(SocialLoginStateServiceTest, OneUseAndOpaqueHandles)
{
    Fixture f;
    const auto binding = f.service->newBinding();
    ASSERT_TRUE(binding);
    const auto state = f.issue();
    EXPECT_TRUE(SocialLoginStateService::validHandle(state));
    EXPECT_NE(state, *binding);
    EXPECT_NE(state, f.service->digest(state));
    EXPECT_EQ(f.consume(state, f.context), Status::Ok);
    EXPECT_EQ(f.consume(state, f.context), Status::Invalid);
}

TEST(SocialLoginStateServiceTest, EveryContextFieldBoundWithoutConsumingOnMismatch)
{
    Fixture f;
    const auto state = f.issue();
    for (int field = 0; field != 5; ++field)
    {
        auto wrong = f.context;
        switch (field)
        {
            case 0: wrong.provider = "google"; break;
            case 1: wrong.clientId = "other-client"; break;
            case 2: wrong.redirectUri += "/other"; break;
            case 3: wrong.bindingHash = std::string(64, 'b'); break;
            case 4: wrong.purpose = "link"; break;
        }
        EXPECT_EQ(f.consume(state, wrong), Status::Invalid);
    }
    EXPECT_EQ(f.consume(state, f.context), Status::Ok);
}

TEST(SocialLoginStateServiceTest, ExactExpiryBoundary)
{
    Fixture f;
    auto state = f.issue();
    f.clock->advanceSeconds(599);
    EXPECT_EQ(f.consume(state, f.context), Status::Ok);
    state = f.issue();
    f.clock->advanceSeconds(600);
    EXPECT_EQ(f.consume(state, f.context), Status::Invalid);
}

TEST(SocialLoginStateServiceTest, ConcurrentConsumersHaveOneWinner)
{
    Fixture f;
    const auto state = f.issue();
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([&] {
            if (f.consume(state, f.context) == Status::Ok) ++winners;
        });
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(winners, 1);
}

TEST(SocialLoginStateServiceTest, StorageAndRandomFailuresFailClosed)
{
    Fixture f;
    const auto state = f.issue();
    f.store->available = false;
    EXPECT_EQ(f.consume(state, f.context), Status::Unavailable);
    f.service->issue(f.context, [](Status status, const std::string &value) {
        EXPECT_EQ(status, Status::Unavailable);
        EXPECT_TRUE(value.empty());
    });
    auto service = std::make_shared<SocialLoginStateService>(f.store, std::make_shared<BrokenCrypto>());
    EXPECT_FALSE(service->newBinding());
    service->issue(f.context, [](Status status, const std::string &value) {
        EXPECT_EQ(status, Status::Unavailable);
        EXPECT_TRUE(value.empty());
    });
}

TEST(SocialLoginStateServiceTest, CollisionDoesNotOverwriteExistingTransaction)
{
    Fixture f;
    const auto first = f.issue();
    f.crypto->reset();
    const auto second = f.issue();
    EXPECT_NE(first, second);
    EXPECT_EQ(f.consume(first, f.context), Status::Ok);
    EXPECT_EQ(f.consume(second, f.context), Status::Ok);
}

TEST(SocialLoginStateServiceTest, RejectMalformedHandles)
{
    Fixture f;
    for (const auto &state : {std::string{}, std::string(63, 'a'), std::string(64, 'A'),
                              std::string(64, 'g'), std::string(65, 'a')})
        EXPECT_EQ(f.consume(state, f.context), Status::Invalid);
}

namespace
{
class CollidingStore : public ISocialLoginStateStore
{
  public:
    int attempts = 0;
    void reserve(const std::string &, const SocialLoginContext &, Callback &&cb) override
    {
        ++attempts;
        cb(Status::Collision);
    }
    void consume(const std::string &, const SocialLoginContext &, Callback &&cb) override
    {
        cb(Status::Invalid);
    }
};
}
TEST(SocialLoginStateServiceTest, CollisionBudgetIsBounded)
{
    Fixture f;
    auto store = std::make_shared<CollidingStore>();
    auto service = std::make_shared<SocialLoginStateService>(store, f.crypto);
    int calls = 0;
    service->issue(f.context, [&](Status status, const std::string &value) {
        ++calls;
        EXPECT_EQ(status, Status::Unavailable);
        EXPECT_TRUE(value.empty());
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(store->attempts, 3);
}
