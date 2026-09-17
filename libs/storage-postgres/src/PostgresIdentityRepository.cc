#include <fulla/storage/postgres/PostgresIdentityRepository.h>

#include <fulla/storage/postgres/models/Users.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/storage/postgres/models/UserRoles.h>
#include <fulla/storage/postgres/models/Oauth2SubjectMappings.h>

#include <drogon/drogon.h>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace drogon_model::fulla_db;
using fulla::identity::UserData;

namespace
{

UserData toUserData(const Users &row)
{
    UserData data;
    data.id = row.getValueOfId();
    data.username = row.getValueOfUsername();
    data.email = row.getValueOfEmail();
    data.passwordHash = row.getValueOfPasswordHash();
    data.salt = row.getValueOfSalt();
    data.publicSub = row.getValueOfPublicSub();
    try
    {
        data.emailVerified = row.getValueOfEmailVerified();
    }
    catch (...)
    {
    }
    try
    {
        data.mfaEnabled = row.getValueOfMfaEnabled();
    }
    catch (...)
    {
    }
    try
    {
        data.mustChangePassword = row.getValueOfMustChangePassword();
    }
    catch (...)
    {
    }
    // V033 profile columns (nullable; getValueOf throws on NULL in some
    // drogon builds, so keep the same defensive shape as the flags above).
    try
    {
        data.displayName = row.getValueOfDisplayName();
    }
    catch (...)
    {
    }
    try
    {
        data.avatarUrl = row.getValueOfAvatarUrl();
    }
    catch (...)
    {
    }
    try
    {
        data.lockedUntil = row.getValueOfLockedUntil();
    }
    catch (...)
    {
    }
    try
    {
        data.failedLoginCount = row.getValueOfFailedLoginCount();
    }
    catch (...)
    {
    }
    return data;
}

}  // namespace

void PostgresIdentityRepository::findByEmail(
  const std::string &email,
  std::function<void(std::optional<UserData>)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt);
        return;
    }
    auto sharedCb =
      std::make_shared<std::function<void(std::optional<UserData>)>>(std::move(callback));
    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findOne(
          Criteria(Users::Cols::_email, CompareOperator::EQ, email) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb](const Users &row) { (*sharedCb)(toUserData(row)); },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

void PostgresIdentityRepository::findByUsername(
  const std::string &username,
  std::function<void(std::optional<UserData>)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt);
        return;
    }
    auto sharedCb =
      std::make_shared<std::function<void(std::optional<UserData>)>>(std::move(callback));
    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findOne(
          Criteria(Users::Cols::_username, CompareOperator::EQ, username) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb](const Users &row) { (*sharedCb)(toUserData(row)); },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

void PostgresIdentityRepository::findById(
  int32_t userId,
  std::function<void(std::optional<UserData>)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt);
        return;
    }
    auto sharedCb =
      std::make_shared<std::function<void(std::optional<UserData>)>>(std::move(callback));
    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findOne(
          Criteria(Users::Cols::_id, CompareOperator::EQ, userId) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb](const Users &row) { (*sharedCb)(toUserData(row)); },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

void PostgresIdentityRepository::findByPublicSub(
  const std::string &publicSub,
  std::function<void(std::optional<UserData>)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt);
        return;
    }
    auto sharedCb =
      std::make_shared<std::function<void(std::optional<UserData>)>>(std::move(callback));
    try
    {
        Mapper<Users> mapper(dbClient_);
        mapper.findOne(
          Criteria(Users::Cols::_public_sub, CompareOperator::EQ, publicSub) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb](const Users &row) { (*sharedCb)(toUserData(row)); },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

void PostgresIdentityRepository::create(
  const UserData &userData,
  std::function<void(std::optional<int32_t>, std::string)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt, "INTERNAL_ERROR");
        return;
    }

    Users newUser;
    if (!userData.username.empty())
        newUser.setUsername(userData.username);
    newUser.setPasswordHash(userData.passwordHash);
    newUser.setSalt(userData.salt);
    if (!userData.email.empty())
        newUser.setEmail(userData.email);

    auto sharedCb = std::make_shared<std::function<void(std::optional<int32_t>, std::string)>>(
      std::move(callback)
    );
    auto db = dbClient_;

    try
    {
        Mapper<Users> mapper(db);
        mapper.insert(
          newUser,
          [sharedCb, db](const Users &inserted) {
              int32_t newUserId = inserted.getValueOfId();

              // #143: every completion path must end with a (local, <id>)
              // subject mapping — consent's getInternalUserId resolves users
              // exclusively through that table and 500s without a row. A
              // mapping failure is logged but does not fail user creation
              // (same tolerance as the role assignment below); the startup
              // backfill converges the invariant.
              auto ensureMapping = [sharedCb, db, newUserId]() {
                  try
                  {
                      db->execSqlAsync(
                        "INSERT INTO oauth2_subject_mappings "
                        "(subject, internal_user_id, provider) "
                        "VALUES ($1, $2, 'local') "
                        "ON CONFLICT (provider, subject) DO NOTHING",
                        [sharedCb, newUserId](const Result &) {
                            (*sharedCb)(newUserId, "");
                        },
                        [sharedCb, newUserId](const DrogonDbException &e) {
                            LOG_ERROR << "PostgresIdentityRepository::create: subject "
                                         "mapping insert failed for user "
                                      << newUserId << ": " << e.base().what();
                            (*sharedCb)(newUserId, "");
                        },
                        std::to_string(newUserId), newUserId);
                  }
                  catch (...)
                  {
                      LOG_ERROR << "PostgresIdentityRepository::create: subject mapping "
                                   "exec setup failed for user "
                                << newUserId;
                      (*sharedCb)(newUserId, "");
                  }
              };

              // Assign default "user" role, mirroring
              // OAuth2Server/AuthService.cc::registerUser's existing
              // behavior. A role-assignment failure is logged but does
              // not fail user creation (same tolerance as the original);
              // all paths fall through to ensureMapping().
              try
              {
                  Mapper<Roles> roleMapper(db);
                  roleMapper.findOne(
                    Criteria(Roles::Cols::_name, CompareOperator::EQ, "user"),
                    [sharedCb, db, newUserId, ensureMapping](const Roles &role) {
                        try
                        {
                            Mapper<UserRoles> urMapper(db);
                            UserRoles ur;
                            ur.setUserId(newUserId);
                            ur.setRoleId(role.getValueOfId());
                            urMapper.insert(
                              ur,
                              [ensureMapping](const UserRoles &) { ensureMapping(); },
                              [ensureMapping, newUserId](const DrogonDbException &e) {
                                  // Recoverable: user is already created; role
                                  // assignment is a side effect (callback reports
                                  // success with empty error string).
                                  LOG_WARN << "PostgresIdentityRepository::create: role "
                                              "assignment failed: "
                                           << e.base().what();
                                  ensureMapping();
                              }
                            );
                        }
                        catch (...)
                        {
                            ensureMapping();
                        }
                    },
                    [ensureMapping, newUserId](const DrogonDbException &e) {
                        // Recoverable: user created without a role.
                        LOG_WARN << "PostgresIdentityRepository::create: default role 'user' "
                                    "not found: "
                                 << e.base().what();
                        ensureMapping();
                    }
                  );
              }
              catch (...)
              {
                  ensureMapping();
              }
          },
          [sharedCb](const DrogonDbException &e) {
              const std::string what = e.base().what();
              LOG_ERROR << "PostgresIdentityRepository::create failed: " << what;
              // Classify the failing DB constraint into the same
              // structured Error_Codes OAuth2Server/AuthService.cc's
              // pre-migration registerUser produced (auth-flow-error-
              // code-gaps spec) -- username conflict checked first so a
              // simultaneous username+email conflict reports username.
              if (what.find("users_username_key") != std::string::npos)
                  (*sharedCb)(std::nullopt, "VALIDATION_USERNAME_TAKEN");
              else if (what.find("idx_users_email_unique") != std::string::npos)
                  (*sharedCb)(std::nullopt, "VALIDATION_EMAIL_TAKEN");
              else
                  (*sharedCb)(std::nullopt, "VALIDATION_INVALID_INPUT");
          }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt, "INTERNAL_ERROR");
    }
}

void PostgresIdentityRepository::updatePasswordHash(
  int32_t userId,
  const std::string &newHash,
  std::function<void(bool)> &&callback
)
{
    if (!dbClient_)
    {
        callback(false);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(bool)>>(std::move(callback));
    LOG_DEBUG << "[PG-Identity] updatePasswordHash: userId=" << userId;

    Mapper<Users> mapper(dbClient_);
    mapper.findBy(
      Criteria(Users::Cols::_id, CompareOperator::EQ, userId),
      [sharedCb, newHash, self = shared_from_this()](const std::vector<Users> &users) {
          if (users.empty())
          {
              (*sharedCb)(false);
              return;
          }
          auto updated = std::make_shared<Users>(users[0]);
          updated->setPasswordHash(newHash);
          updated->setSalt("");
          Mapper<Users>(self->dbClient_)
            .update(
              *updated,
              [updated, sharedCb](const size_t) { (*sharedCb)(true); },
              [updated, sharedCb](const DrogonDbException &e) {
                  LOG_WARN << "[PG-Identity] updatePasswordHash FAILED: " << e.base().what();
                  (*sharedCb)(false);
              }
            );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_WARN << "[PG-Identity] updatePasswordHash FAILED: " << e.base().what();
          (*sharedCb)(false);
      }
    );
}

void PostgresIdentityRepository::resetFailedLogins(
  int32_t userId,
  std::function<void(bool)> &&callback
)
{
    if (!dbClient_)
    {
        callback(false);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(bool)>>(std::move(callback));
    LOG_DEBUG << "[PG-Identity] resetFailedLogins: userId=" << userId;

    Mapper<Users> mapper(dbClient_);
    mapper.findBy(
      Criteria(Users::Cols::_id, CompareOperator::EQ, userId),
      [sharedCb, self = shared_from_this()](const std::vector<Users> &users) {
          if (users.empty())
          {
              (*sharedCb)(false);
              return;
          }
          auto updated = std::make_shared<Users>(users[0]);
          updated->setFailedLoginCount(0);
          updated->setLockedUntil(0);
          Mapper<Users>(self->dbClient_)
            .update(
              *updated,
              [updated, sharedCb](const size_t) { (*sharedCb)(true); },
              [updated, sharedCb](const DrogonDbException &e) {
                  LOG_WARN << "[PG-Identity] resetFailedLogins FAILED: " << e.base().what();
                  (*sharedCb)(false);
              }
            );
      },
      [sharedCb](const DrogonDbException &e) {
          LOG_WARN << "[PG-Identity] resetFailedLogins FAILED: " << e.base().what();
          (*sharedCb)(false);
      }
    );
}

void PostgresIdentityRepository::incrementFailedLogins(
  int32_t userId,
  std::function<void(bool)> &&callback
)
{
    if (!dbClient_)
    {
        callback(false);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(bool)>>(std::move(callback));

    Mapper<Users> mapper(dbClient_);
    mapper.findBy(
      Criteria(Users::Cols::_id, CompareOperator::EQ, userId),
      [sharedCb, self = shared_from_this()](const std::vector<Users> &users) {
          int failedCount = users.empty() ? 0 : users[0].getValueOfFailedLoginCount();
          int newFailedCount = failedCount + 1;
          int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
          )
                          .count();
          int64_t newLockedUntil = 0;
          if (newFailedCount >= 20)
              newLockedUntil = now + 3600;
          else if (newFailedCount >= 15)
              newLockedUntil = now + 1800;
          else if (newFailedCount >= 10)
              newLockedUntil = now + 300;
          else if (newFailedCount >= 5)
              newLockedUntil = now + 60;

          if (users.empty())
          {
              (*sharedCb)(false);
              return;
          }
          auto updated = std::make_shared<Users>(users[0]);
          updated->setFailedLoginCount(newFailedCount);
          updated->setLockedUntil(newLockedUntil);
          updated->setLastFailedLogin(now);
          Mapper<Users>(self->dbClient_)
            .update(
              *updated,
              [updated, sharedCb](const size_t) { (*sharedCb)(true); },
              [updated, sharedCb](const DrogonDbException &) { (*sharedCb)(false); }
            );
      },
      [sharedCb](const DrogonDbException &) { (*sharedCb)(false); }
    );
}

void PostgresIdentityRepository::getUserInfoWithRoles(
  int32_t userId,
  std::function<void(std::optional<Json::Value>)> &&callback
)
{
    if (!dbClient_)
    {
        callback(std::nullopt);
        return;
    }
    auto sharedCb =
      std::make_shared<std::function<void(std::optional<Json::Value>)>>(std::move(callback));
    auto db = dbClient_;

    try
    {
        Mapper<Users>(db).findBy(
          Criteria(Users::Cols::_id, CompareOperator::EQ, userId) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [sharedCb, db, userId](const std::vector<Users> &users) {
              if (users.empty())
              {
                  (*sharedCb)(std::nullopt);
                  return;
              }
              const auto &user = users[0];

              // Split JOIN: UserRoles::findBy + Roles::findBy for each role
              Mapper<UserRoles>(db).findBy(
                Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, userId),
                [sharedCb, db, user](const std::vector<UserRoles> &userRoles) {
                    if (userRoles.empty())
                    {
                        Json::Value json;
                        json["sub"] = user.getValueOfPublicSub();
                        std::string dn = user.getValueOfUsername();
                        {
                            // V033: display_name preferred for `name` when set.
                            std::string dp = user.getValueOfDisplayName();
                            json["name"] = !dp.empty() ? dp
                                                        : (dn.empty() ? user.getValueOfEmail() : dn);
                            if (!user.getValueOfAvatarUrl().empty())
                                json["picture"] = user.getValueOfAvatarUrl();
                        }
                        json["email"] = user.getValueOfEmail();
                        json["roles"] = Json::Value(Json::arrayValue);
                        (*sharedCb)(json);
                        return;
                    }

                    std::vector<int32_t> roleIds;
                    for (const auto &ur : userRoles)
                        roleIds.push_back(ur.getValueOfRoleId());

                    Mapper<Roles>(db).findBy(
                      Criteria(Roles::Cols::_id, CompareOperator::In, roleIds),
                      [sharedCb, user](const std::vector<Roles> &roles) {
                          Json::Value json;
                          json["sub"] = user.getValueOfPublicSub();
                          std::string dn = user.getValueOfUsername();
                          {
                              // V033: display_name preferred for `name` when set.
                              std::string dp = user.getValueOfDisplayName();
                              json["name"] =
                                !dp.empty() ? dp : (dn.empty() ? user.getValueOfEmail() : dn);
                              if (!user.getValueOfAvatarUrl().empty())
                                  json["picture"] = user.getValueOfAvatarUrl();
                          }
                          json["email"] = user.getValueOfEmail();
                          Json::Value rj(Json::arrayValue);
                          for (const auto &r : roles)
                              rj.append(r.getValueOfName());
                          json["roles"] = rj;
                          (*sharedCb)(json);
                      },
                      [sharedCb, user](const DrogonDbException &) {
                          Json::Value json;
                          json["sub"] = user.getValueOfPublicSub();
                          std::string dn = user.getValueOfUsername();
                          {
                              // V033: display_name preferred for `name` when set.
                              std::string dp = user.getValueOfDisplayName();
                              json["name"] =
                                !dp.empty() ? dp : (dn.empty() ? user.getValueOfEmail() : dn);
                              if (!user.getValueOfAvatarUrl().empty())
                                  json["picture"] = user.getValueOfAvatarUrl();
                          }
                          json["email"] = user.getValueOfEmail();
                          json["roles"] = Json::Value(Json::arrayValue);
                          (*sharedCb)(json);
                      }
                    );
                },
                [sharedCb, user](const DrogonDbException &) {
                    Json::Value json;
                    json["sub"] = user.getValueOfPublicSub();
                    std::string dn = user.getValueOfUsername();
                    {
                        // V033: display_name preferred for `name` when set.
                        std::string dp = user.getValueOfDisplayName();
                        json["name"] = !dp.empty() ? dp : (dn.empty() ? user.getValueOfEmail() : dn);
                        if (!user.getValueOfAvatarUrl().empty())
                            json["picture"] = user.getValueOfAvatarUrl();
                    }
                    json["email"] = user.getValueOfEmail();
                    json["roles"] = Json::Value(Json::arrayValue);
                    (*sharedCb)(json);
                }
              );
          },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

void PostgresIdentityRepository::getRoles(
  int32_t internalUserId,
  std::function<void(std::vector<std::string>)> &&cb
)
{
    if (!dbClient_)
    {
        cb({});
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(cb));
    auto db = dbClient_;

    try
    {
        Mapper<UserRoles> urMapper(db);
        urMapper.findBy(
          Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, internalUserId),
          [sharedCb, db](const std::vector<UserRoles> &userRoles) {
              if (userRoles.empty())
              {
                  (*sharedCb)({});
                  return;
              }
              std::vector<int32_t> roleIds;
              for (const auto &ur : userRoles)
                  roleIds.push_back(ur.getValueOfRoleId());

              Mapper<Roles> roleMapper(db);
              roleMapper.findBy(
                Criteria(Roles::Cols::_id, CompareOperator::In, roleIds),
                [sharedCb](const std::vector<Roles> &roles) {
                    std::vector<std::string> names;
                    for (const auto &role : roles)
                        names.push_back(role.getValueOfName());
                    (*sharedCb)(names);
                },
                [sharedCb](const DrogonDbException &) { (*sharedCb)({}); }
              );
          },
          [sharedCb](const DrogonDbException &) { (*sharedCb)({}); }
        );
    }
    catch (...)
    {
        (*sharedCb)({});
    }
}

void PostgresIdentityRepository::getInternalUserId(
  const std::string &subject,
  const std::string &provider,
  std::function<void(std::optional<int32_t>)> &&cb
)
{
    if (!dbClient_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(std::optional<int32_t>)>>(std::move(cb));
    try
    {
        Mapper<Oauth2SubjectMappings> mapper(dbClient_);
        mapper.findOne(
          Criteria(Oauth2SubjectMappings::Cols::_provider, CompareOperator::EQ, provider) &&
            Criteria(Oauth2SubjectMappings::Cols::_subject, CompareOperator::EQ, subject),
          [db = dbClient_, sharedCb](const Oauth2SubjectMappings &mapping) {
              // #54 liveness check: the mapping may outlive its user (soft
              // delete intentionally keeps the mapping row). This resolution
              // feeds the consent → authorization-code → token chain
              // (SessionController), so a dead user's mapping must NOT mint
              // fresh tokens. Split query (JOIN-forbidden): resolve the
              // mapped id against a live users row; not live → nullopt
              // (SessionController fails the consent with an error, it does
              // NOT attempt account creation). Locked users are not filtered
              // here — that is login-entry policy, not mapping resolution.
              int32_t internalId = mapping.getValueOfInternalUserId();
              try
              {
                  Mapper<Users> usersMapper(db);
                  usersMapper.findBy(
                    Criteria(Users::Cols::_id, CompareOperator::EQ, internalId) &&
                      Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
                    [sharedCb, internalId](const std::vector<Users> &users) {
                        (*sharedCb)(users.empty() ? std::nullopt : std::make_optional(internalId));
                    },
                    [sharedCb](const DrogonDbException &e) {
                        LOG_ERROR << "getInternalUserId: liveness query failed: "
                                  << e.base().what();
                        (*sharedCb)(std::nullopt);
                    }
                  );
              }
              catch (...)
              {
                  LOG_ERROR << "getInternalUserId: users Mapper construction failed";
                  (*sharedCb)(std::nullopt);
              }
          },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); }
        );
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

// Phase 1.5b (Task 39): subject-string overload. Ported from the legacy
// oauth2::PostgresRoleRepository::getUserRoles(string) -- numeric subject is
// used directly as the internal id; otherwise treated as users.public_sub and
// resolved to the internal id first, then the int32 path is reused.
void PostgresIdentityRepository::getRoles(
  const std::string &subject,
  std::function<void(std::vector<std::string>)> &&cb
)
{
    if (!dbClient_)
    {
        cb({});
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(cb));
    auto db = dbClient_;

    // Numeric subject -> internal id directly.
    bool isNumeric = false;
    int32_t numericId = 0;
    try
    {
        size_t pos = 0;
        int parsed = std::stoi(subject, &pos);
        isNumeric = (pos == subject.length());
        if (isNumeric)
            numericId = parsed;
    }
    catch (...)
    {
        isNumeric = false;
    }

    if (isNumeric)
    {
        getRoles(numericId, [sharedCb](std::vector<std::string> roles) { (*sharedCb)(roles); });
        return;
    }

    // Otherwise resolve public_sub -> internal id, then the int32 path.
    try
    {
        Mapper<Users> userMapper(db);
        userMapper.findOne(
          Criteria(Users::Cols::_public_sub, CompareOperator::EQ, subject) &&
            Criteria(Users::Cols::_deleted_at, CompareOperator::IsNull),
          [this, sharedCb](const Users &user) {
              getRoles(user.getValueOfId(), [sharedCb](std::vector<std::string> roles) {
                  (*sharedCb)(roles);
              });
          },
          [sharedCb](const DrogonDbException &) { (*sharedCb)({}); }
        );
    }
    catch (...)
    {
        (*sharedCb)({});
    }
}

// Phase 1.5b (Task 39): write path, ported from the legacy
// oauth2::PostgresSubjectMappingRepository.
void PostgresIdentityRepository::createSubjectMapping(
  const std::string &subject,
  int32_t internalUserId,
  const std::string &provider,
  std::function<void(bool)> &&cb
)
{
    if (!dbClient_)
    {
        cb(false);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(bool)>>(std::move(cb));
    try
    {
        Mapper<Oauth2SubjectMappings> mapper(dbClient_);
        Oauth2SubjectMappings mapping;
        mapping.setSubject(subject);
        mapping.setInternalUserId(internalUserId);
        mapping.setProvider(provider);
        mapper.insert(
          mapping,
          [sharedCb](const Oauth2SubjectMappings &) { (*sharedCb)(true); },
          [sharedCb](const DrogonDbException &) { (*sharedCb)(false); }
        );
    }
    catch (...)
    {
        (*sharedCb)(false);
    }
}

// Phase 1.5b (Task 39): raw SQL INSERT ... ON CONFLICT ... RETURNING is the
// upsert exemption in .claude/rules/db-operations.md (the Mapper cannot
// express ON CONFLICT). Ported verbatim from the legacy
// oauth2::PostgresSubjectMappingRepository::createUserForExternalLogin.
void PostgresIdentityRepository::createUserForExternalLogin(
  const std::string &externalId,
  const std::string &provider,
  std::function<void(std::optional<int32_t>)> &&cb
)
{
    if (!dbClient_)
    {
        cb(std::nullopt);
        return;
    }
    auto sharedCb = std::make_shared<std::function<void(std::optional<int32_t>)>>(std::move(cb));

    std::string username = provider + "_" + externalId.substr(0, 20);
    // #54: DO NOTHING (fail-closed). The previous DO UPDATE SET username =
    // users.username was adoption by another name — a conflicting row
    // (whoever registered that username, or a soft-deleted user) would be
    // silently linked to this external identity. A conflict now returns no
    // row and the caller gets nullopt.
    dbClient_->execSqlAsync(
      "INSERT INTO users (username, password_hash, salt, email) "
      "VALUES ($1, 'EXTERNAL_AUTH_NO_PASSWORD', '', '') "
      "ON CONFLICT (username) DO NOTHING "
      "RETURNING id",
      [sharedCb](const Result &r) {
          if (r.empty())
          {
              (*sharedCb)(std::nullopt);
              return;
          }
          (*sharedCb)(r[0]["id"].as<int32_t>());
      },
      [sharedCb](const DrogonDbException &) { (*sharedCb)(std::nullopt); },
      username
    );
}

}  // namespace fulla::storage::postgres
