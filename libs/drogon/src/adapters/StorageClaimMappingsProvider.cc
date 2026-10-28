#include <fulla/drogon/adapters/StorageClaimMappingsProvider.h>

#include <fulla/storage/postgres/models/ClaimMappings.h>
#include <fulla/storage/postgres/models/Users.h>

#include <trantor/utils/Logger.h>

#include <sstream>

namespace fulla::drogon::adapters
{

using namespace ::drogon::orm;
using MappingModel = ::drogon_model::fulla_db::ClaimMappings;
using UserModel = ::drogon_model::fulla_db::Users;

StorageClaimMappingsProvider::StorageClaimMappingsProvider(
  ::drogon::orm::DbClientPtr dbClient
)
    : dbClient_(std::move(dbClient))
{
}

StorageClaimMappingsProvider::~StorageClaimMappingsProvider() = default;

void StorageClaimMappingsProvider::getEnabledMappings(MappingsCallback &&cb)
{
    auto sharedCb = std::make_shared<MappingsCallback>(std::move(cb));
    if (!dbClient_)
    {
        (*sharedCb)({});
        return;
    }
    try
    {
        Mapper<MappingModel> mapper(dbClient_);
        mapper.findBy(
          Criteria(MappingModel::Cols::_enabled, CompareOperator::EQ, true),
          [sharedCb](const std::vector<MappingModel> &rows) {
              std::vector<fulla::common::ports::ClaimMappingEntry> out;
              out.reserve(rows.size());
              for (const auto &row : rows)
              {
                  fulla::common::ports::ClaimMappingEntry e;
                  e.scopeName = row.getValueOfScopeName();
                  e.claimName = row.getValueOfClaimName();
                  e.sourcePath = row.getValueOfSourcePath();
                  e.includeIn = row.getValueOfIncludeIn();
                  out.push_back(std::move(e));
              }
              (*sharedCb)(std::move(out));
          },
          [sharedCb](const DrogonDbException &e) {
              // Claim data: failure mode is absence (org_ctx semantics).
              LOG_WARN << "ClaimMappingsProvider lookup failed: " << e.base().what();
              (*sharedCb)({});
          });
    }
    catch (...)
    {
        LOG_WARN << "ClaimMappingsProvider: Mapper construction failed";
        (*sharedCb)({});
    }
}

void resolveUserProfileJson(
  const ::drogon::orm::DbClientPtr &db,
  const std::string &subject,
  ProfileJsonCallback &&cb
)
{
    auto sharedCb = std::make_shared<ProfileJsonCallback>(std::move(cb));
    if (!db)
    {
        (*sharedCb)(std::nullopt);
        return;
    }
    // Dual-key subject resolution (the /api/me canon, V024): all-digits
    // resolves by internal id, anything else by users.public_sub.
    Criteria crit = [&] {
        if (!subject.empty() &&
            subject.find_first_not_of("0123456789") == std::string::npos)
            return Criteria(UserModel::Cols::_id, CompareOperator::EQ,
                            static_cast<int32_t>(std::stoi(subject)));
        return Criteria(UserModel::Cols::_public_sub, CompareOperator::EQ, subject);
    }();
    // Soft-deleted rows never resolve a profile.
    crit = crit && Criteria(UserModel::Cols::_deleted_at, CompareOperator::IsNull);

    try
    {
        Mapper<UserModel> mapper(db);
        mapper.findOne(
          crit,
          [sharedCb](const UserModel &user) {
              // The scalar source-path walk needs exactly two roots:
              // display_name and custom_attributes (stored as a JSONB
              // string in the generated model — reparse it into the
              // composed document).
              Json::Value profile;
              const std::string display = user.getValueOfDisplayName();
              if (!display.empty())
                  profile["display_name"] = display;
              const std::string attrs = user.getValueOfCustomAttributes();
              if (!attrs.empty())
              {
                  Json::CharReaderBuilder rb;
                  std::string errs;
                  Json::Value parsed;
                  std::istringstream in(attrs);
                  if (Json::parseFromStream(rb, in, &parsed, &errs) && parsed.isObject())
                      profile["custom_attributes"] = parsed;
              }
              Json::StreamWriterBuilder wb;
              wb["indentation"] = "";
              (*sharedCb)(Json::writeString(wb, profile));
          },
          [sharedCb](const DrogonDbException &) {
              (*sharedCb)(std::nullopt);
          });
    }
    catch (...)
    {
        (*sharedCb)(std::nullopt);
    }
}

}  // namespace fulla::drogon::adapters
