#include "AdminBootstrapper.h"

#include <fulla/storage/postgres/models/Oauth2SubjectMappings.h>
#include <fulla/storage/postgres/models/Roles.h>
#include <fulla/storage/postgres/models/UserRoles.h>
#include <fulla/storage/postgres/models/Users.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::fulla_db;

namespace bootstrap
{
namespace
{
// Same PBKDF2-SHA256 parameters/format as identity AuthService::hashPassword
// ("$pbkdf2-sha256$310000$<hexsalt>$<hexhash>"). Kept local so the
// bootstrapper does not depend on identity service wiring order.
constexpr int kIterations = 310000;
constexpr int kSaltLength = 16;
constexpr int kKeyLength = 32;

std::string bytesToHex(const unsigned char *data, size_t len)
{
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++)
    {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }
    return out;
}

std::string hashPasswordPbkdf2(const std::string &password)
{
    unsigned char salt[kSaltLength];
    if (RAND_bytes(salt, kSaltLength) != 1)
        throw std::runtime_error("AdminBootstrapper: RAND_bytes failed");
    unsigned char key[kKeyLength];
    if (
      PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()), salt, kSaltLength, kIterations,
        EVP_sha256(), kKeyLength, key
      ) != 1
    )
        throw std::runtime_error("AdminBootstrapper: PBKDF2 failed");
    return "$pbkdf2-sha256$" + std::to_string(kIterations) + "$" + bytesToHex(salt, kSaltLength) +
           "$" + bytesToHex(key, kKeyLength);
}

std::string randomPassword()
{
    unsigned char raw[24];
    if (RAND_bytes(raw, sizeof(raw)) != 1)
        throw std::runtime_error("AdminBootstrapper: RAND_bytes failed");
    // Printable, unambiguous alphabet; no dependency on a base64 lib.
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    std::string out;
    out.reserve(31);
    for (size_t i = 0; i < sizeof(raw); i++)
    {
        out.push_back(alphabet[raw[i] % 56]);
        if ((i + 1) % 8 == 0 && i + 1 < sizeof(raw))
            out.push_back('-');
    }
    return out;
}

// Shared per-run state threaded through the async steps.
struct BootstrapRun
{
    AdminBootstrapper::DoneCallback done;
    DbClientPtr db;
    std::string explicitPassword;  // may be empty -> random
    int32_t roleId = 0;
    int32_t userId = 0;
};

using RunPtr = std::shared_ptr<BootstrapRun>;

void finish(const RunPtr &run, bool created, const std::string &detail)
{
    run->done(created, detail);
}

// Step 4: local subject mapping (subject = internal id text, provider
// 'local' — the seed/dev_admin_user.sql shape the consent/login flows read).
void ensureSubjectMapping(const RunPtr &run)
{
    try
    {
        Mapper<Oauth2SubjectMappings> mapper(run->db);
        mapper.findOne(
          Criteria(Oauth2SubjectMappings::Cols::_provider, CompareOperator::EQ, std::string("local")) &&
            Criteria(Oauth2SubjectMappings::Cols::_subject, CompareOperator::EQ, std::to_string(run->userId)),
          [run](const Oauth2SubjectMappings &) { finish(run, true, "admin bootstrapped"); },
          [run](const DrogonDbException &) {
              try
              {
                  Mapper<Oauth2SubjectMappings> mapper(run->db);
                  Oauth2SubjectMappings mapping;
                  mapping.setSubject(std::to_string(run->userId));
                  mapping.setInternalUserId(run->userId);
                  mapping.setProvider("local");
                  mapper.insert(
                    mapping,
                    [run](const Oauth2SubjectMappings &) { finish(run, true, "admin bootstrapped"); },
                    [run](const DrogonDbException &e2) {
                        finish(run, false, std::string("subject mapping insert failed: ") + e2.base().what());
                    }
                  );
              }
              catch (const DrogonDbException &e2)
              {
                  finish(run, false, std::string("mapping mapper failed: ") + e2.base().what());
              }
          }
        );
    }
    catch (const DrogonDbException &e)
    {
        finish(run, false, std::string("mapping mapper failed: ") + e.base().what());
    }
}

// Step 3: admin role assignment for the resolved user.
void ensureAdminRole(const RunPtr &run)
{
    try
    {
        Mapper<UserRoles> urMapper(run->db);
        urMapper.findOne(
          Criteria(UserRoles::Cols::_user_id, CompareOperator::EQ, run->userId) &&
            Criteria(UserRoles::Cols::_role_id, CompareOperator::EQ, run->roleId),
          [run](const UserRoles &) { ensureSubjectMapping(run); },
          [run](const DrogonDbException &) {
              try
              {
                  Mapper<UserRoles> urMapper(run->db);
                  UserRoles ur;
                  ur.setUserId(run->userId);
                  ur.setRoleId(run->roleId);
                  urMapper.insert(
                    ur,
                    [run](const UserRoles &) { ensureSubjectMapping(run); },
                    [run](const DrogonDbException &e2) {
                        finish(run, false, std::string("admin role assignment failed: ") + e2.base().what());
                    }
                  );
              }
              catch (const DrogonDbException &e2)
              {
                  finish(run, false, std::string("user_roles mapper failed: ") + e2.base().what());
              }
          }
        );
    }
    catch (const DrogonDbException &e)
    {
        finish(run, false, std::string("user_roles mapper failed: ") + e.base().what());
    }
}

// Step 2b: the user row exists (created now or pre-existing) — proceed to
// the role step.
void resolveUserById(const RunPtr &run)
{
    ensureAdminRole(run);
}

// Step 2: the 'admin' user row. Created here when missing; an existing row
// is REUSED (partial-failure self-heal: a previous run may have inserted the
// user but died before the role/mapping steps).
void ensureAdminUser(const RunPtr &run)
{
    try
    {
        Mapper<Users> userMapper(run->db);
        userMapper.findOne(
          Criteria(Users::Cols::_username, CompareOperator::EQ, std::string("admin")),
          [run](const Users &existing) {
              run->userId = existing.getValueOfId();
              resolveUserById(run);
          },
          [run](const DrogonDbException &) {
              const bool generated = run->explicitPassword.empty();
              const std::string password = generated ? randomPassword() : run->explicitPassword;
              std::string passwordHash;
              try
              {
                  passwordHash = hashPasswordPbkdf2(password);
              }
              catch (const std::exception &e)
              {
                  finish(run, false, e.what());
                  return;
              }

                  try
                  {
                      Mapper<Users> userMapper(run->db);
                      Users admin;
                      admin.setUsername("admin");
                      admin.setPasswordHash(passwordHash);
                      // users.salt is NOT NULL: PBKDF2 embeds the salt in the
                      // hash string, so the column carries the empty string
                      // (same convention as every other creation path). Missing
                      // this made the first-boot insert fail on the constraint.
                      admin.setSalt("");
                      // Bootstrap admin email policy: FULLA_BOOTSTRAP_ADMIN_EMAIL
                      // points the account at a real, operator-owned mailbox. When
                      // it is set AND real SMTP delivery is configured (same
                      // HOST/USER/PASSWORD rule as utils/EmailService.cc), the row
                      // is born UNVERIFIED — the #145 forced password change then
                      // triggers the verification email, and the login flow's
                      // email-verified gate holds until the operator clicks the
                      // link. Without a deliverable mailbox (placeholder + no
                      // SMTP), the row ships verified=true instead: nothing could
                      // ever deliver a verification email, so the gate would
                      // deadlock the administrator (prod rollout 2026-09-14).
                      const char *envEmail = std::getenv("FULLA_BOOTSTRAP_ADMIN_EMAIL");
                      const char *smtpHost = std::getenv("FULLA_SMTP_HOST");
                      const char *smtpUser = std::getenv("FULLA_SMTP_USER");
                      const char *smtpPass = std::getenv("FULLA_SMTP_PASSWORD");
                      const bool realMailbox =
                        envEmail && envEmail[0] != 0 && smtpHost && smtpHost[0] != 0 &&
                        smtpUser && smtpUser[0] != 0 && smtpPass && smtpPass[0] != 0;
                      if (realMailbox)
                      {
                          admin.setEmail(envEmail);
                          admin.setEmailVerified(false);
                          LOG_INFO << "Bootstrap: admin email set to " << envEmail
                                   << " (unverified — a verification email will be"
                                   << " sent after the first password change)";
                      }
                      else
                      {
                          admin.setEmail("admin@example.com");
                          admin.setEmailVerified(true);
                      }
                      // #145: force a password change at first login. Both the
                      // random password (printed to the log) and an
                      // operator-provided env password must be replaced by the
                      // administrator; while flagged, no authorization codes
                      // are issued for the account (POST /oauth2/password/change
                      // is the designated change path).
                      admin.setMustChangePassword(true);
                      userMapper.insert(
                        admin,
                        [run, generated, password](const Users &inserted) {
                        run->userId = inserted.getValueOfId();
                        if (generated)
                        {
                            // Print AFTER the insert succeeded: a failed insert
                            // must not claim a credential was minted.
                            LOG_WARN << "==========================================================";
                            LOG_WARN << "Bootstrap: created administrator 'admin' with password: " << password;
                            LOG_WARN << "SAVE IT NOW and change it after first login. It is shown ONCE.";
                            LOG_WARN << "==========================================================";
                        }
                        else
                        {
                            LOG_INFO << "Bootstrap: created administrator 'admin' (password from env)";
                        }
                        resolveUserById(run);
                    },
                    [run](const DrogonDbException &e2) {
                        finish(run, false, std::string("admin user insert failed: ") + e2.base().what());
                    }
                  );
              }
              catch (const DrogonDbException &e2)
              {
                  finish(run, false, std::string("users mapper failed: ") + e2.base().what());
              }
          }
        );
    }
    catch (const DrogonDbException &e)
    {
        finish(run, false, std::string("users mapper failed: ") + e.base().what());
    }
}
}  // namespace

void AdminBootstrapper::run(const std::string &explicitPassword, DoneCallback &&done)
{
    auto run = std::make_shared<BootstrapRun>();
    run->done = std::move(done);
    run->explicitPassword = explicitPassword;
    run->db = app().getDbClient();
    if (!run->db)
    {
        finish(run, false, "no db client (memory storage?)");
        return;
    }

    try
    {
        Mapper<Roles> roleMapper(run->db);
        roleMapper.findOne(
          Criteria(Roles::Cols::_name, CompareOperator::EQ, std::string("admin")),
          [run](const Roles &adminRole) {
              run->roleId = adminRole.getValueOfId();
              ensureAdminUser(run);
          },
          [run](const DrogonDbException &e) {
              finish(
                run, false,
                std::string("role 'admin' not found (roles not seeded?): ") + e.base().what()
              );
          }
        );
    }
    catch (...)
    {
        finish(run, false, "INTERNAL_ERROR");
    }
}
void AdminBootstrapper::backfillLocalSubjectMappings(DoneCallback &&done)
{
    auto db = app().getDbClient();
    if (!db)
    {
        done(false, "no db client (memory storage?)");
        return;
    }
    try
    {
        db->execSqlAsync(
          "INSERT INTO oauth2_subject_mappings (subject, internal_user_id, provider) "
          "SELECT u.id::text, u.id, 'local' FROM users u "
          "ON CONFLICT (provider, subject) DO NOTHING "
          "RETURNING 1",
          [done](const Result &r) {
              if (r.size() > 0)
                  LOG_INFO << "SubjectMappingBackfill: healed " << r.size() << " user(s)";
              done(true, "local subject mapping invariant holds");
          },
          [done](const DrogonDbException &e) {
              done(false, std::string("subject mapping backfill failed: ") + e.base().what());
          }
        );
    }
    catch (...)
    {
        done(false, "INTERNAL_ERROR");
    }
}

}  // namespace bootstrap
