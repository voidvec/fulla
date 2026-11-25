#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/storage/postgres/models/Users.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <drogon/utils/Utilities.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

using namespace drogon;
using namespace drogon::orm;

DROGON_TEST(Integration_P0_UserSystem_General_Works)
{
    // Skip this test in memory storage mode (no database)
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        LOG_INFO << "Skipping UserSystemTest in memory storage mode";
        return;
    }

    auto db = app().getDbClient();
    if (!db)
    {
        LOG_WARN << "DB Client unavailable, skipping UserSystemTest";
        return;
    }

    // Schema defined in sql/migrations/V004__users_table.sql

    // Clean up
    try
    {
        db->execSqlSync("DELETE FROM users WHERE username = $1", "unittest_user_orm");
    }
    catch (...)
    {
    }

    // 1. Create User Data (ORM)
    drogon_model::fulla_db::Users newUser;
    newUser.setUsername("unittest_user_orm");
    std::string password = "password123";
    std::string salt = utils::getUuid();
    std::string hash = utils::getSha256(password + salt);
    newUser.setPasswordHash(hash);
    newUser.setSalt(salt);
    newUser.setEmail("test_orm@example.com");

    auto mapper = Mapper<drogon_model::fulla_db::Users>(db);

    // #72: rerun-safety -- the tail cleanup only runs when the test completes;
    // a crashed prior run leaves the fixed username/email behind and the
    // unique index turns this INSERT into a FAIL. Clear the slot first.
    try
    {
        db->execSqlSync("DELETE FROM users WHERE username = $1", "unittest_user_orm");
    }
    catch (...)
    {
    }

    // 2. Insert (ORM)
    try
    {
        mapper.insert(newUser);
        LOG_INFO << "User inserted via ORM";
    }
    catch (const DrogonDbException &e)
    {
        LOG_ERROR << "ORM Insert Error: " << e.base().what();
        FAIL("ORM Insert Failed");
    }

    // Delay to ensure persistence
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Raw SQL Verification
    {
        auto result =
          db->execSqlSync("SELECT * FROM users WHERE username = $1", "unittest_user_orm");
        if (result.empty())
        {
            LOG_ERROR << "Raw SQL: User NOT found in database!";
        }
        else
        {
            LOG_INFO << "Raw SQL: User found! ID: " << result[0]["id"].as<int>();
        }
    }

    // 3. Verify (ORM)
    try
    {
        LOG_INFO << "Attempting to find user...";
        auto user = mapper.findOne(Criteria(
          drogon_model::fulla_db::Users::Cols::_username, CompareOperator::EQ, "unittest_user_orm"
        ));
        LOG_INFO << "User found via ORM!";

        std::string dbHash = user.getValueOfPasswordHash();
        std::string dbSalt = user.getValueOfSalt();

        CHECK(dbSalt == salt);

        std::string inputHash = utils::getSha256(password + dbSalt);
        std::transform(dbHash.begin(), dbHash.end(), dbHash.begin(), ::tolower);
        std::transform(inputHash.begin(), inputHash.end(), inputHash.begin(), ::tolower);

        CHECK(dbHash == inputHash);
    }
    catch (const DrogonDbException &e)
    {
        LOG_ERROR << "FindOne Error: " << e.base().what();
        FAIL("User not found via ORM: " + std::string(e.base().what()));
    }

    // Clean up
    try
    {
        db->execSqlSync("DELETE FROM users WHERE username = $1", "unittest_user_orm");
    }
    catch (...)
    {
    }
}

using namespace drogon;
