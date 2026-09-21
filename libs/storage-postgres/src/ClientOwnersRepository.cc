// See ClientOwnersRepository.h for the design rationale (#222/#230,
// v1.5.0 M0). All queries are async callback + Mapper + Criteria with an
// independent try/catch around every Mapper construction (db-operations.md);
// every failure path reaches (*sharedCb).

#include <fulla/storage/postgres/ClientOwnersRepository.h>

#include <drogon/orm/Mapper.h>

#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/Users.h>

#include <memory>
#include <vector>

namespace fulla::storage::postgres
{

using namespace ::drogon::orm;
using namespace drogon_model::fulla_db;

ClientOwnersRepository::ClientOwnersRepository(::drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
}

namespace
{

// #230: only "no rows" is a legitimate NoRow outcome. Everything else
// (connection failure, schema drift, ...) is a real error the caller must
// surface instead of silently treating as "no owner row" (which used to
// turn a broken DB into a uniform 404).
OwnerRowLookup ownerRowFromException(const DrogonDbException &e)
{
    OwnerRowLookup lookup;
    if (dynamic_cast<const UnexpectedRows *>(&e) != nullptr)
    {
        lookup.status = LookupStatus::NoRow;
    }
    else
    {
        lookup.status = LookupStatus::Error;
        lookup.error = e.base().what();
    }
    return lookup;
}

}  // namespace

void ClientOwnersRepository::findOwnerRow(const std::string &clientId, OwnerRowCallback &&cb)
{
    auto sharedCb = std::make_shared<OwnerRowCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2ClientOwners> mapper(dbClient_);
        mapper.findOne(
          Criteria(Oauth2ClientOwners::Cols::_client_id, CompareOperator::EQ, clientId),
          [sharedCb](const Oauth2ClientOwners &owner) {
              OwnerRowLookup lookup;
              lookup.status = LookupStatus::Found;
              lookup.row = owner;
              (*sharedCb)(lookup);
          },
          [sharedCb](const DrogonDbException &e) {
              (*sharedCb)(ownerRowFromException(e));
          }
        );
    }
    catch (const std::exception &e)
    {
        OwnerRowLookup lookup;
        lookup.status = LookupStatus::Error;
        lookup.error = e.what();
        (*sharedCb)(lookup);
    }
    catch (...)
    {
        OwnerRowLookup lookup;
        lookup.status = LookupStatus::Error;
        lookup.error = "owner row lookup: Mapper construction failed (unknown exception)";
        (*sharedCb)(lookup);
    }
}

void ClientOwnersRepository::findMembership(int32_t orgId, int32_t userId, MembershipCallback &&cb)
{
    auto sharedCb = std::make_shared<MembershipCallback>(std::move(cb));
    try
    {
        Mapper<OrganizationMembers> mapper(dbClient_);
        mapper.findBy(
          Criteria(OrganizationMembers::Cols::_organization_id, CompareOperator::EQ, orgId) &&
              Criteria(OrganizationMembers::Cols::_user_id, CompareOperator::EQ, userId),
          [sharedCb](const std::vector<OrganizationMembers> &rows) {
              MembershipLookup lookup;
              if (rows.empty())
              {
                  lookup.status = LookupStatus::NoRow;
              }
              else
              {
                  // UNIQUE (organization_id, user_id) makes rows[0] the row.
                  lookup.status = LookupStatus::Found;
                  lookup.row = rows[0];
              }
              (*sharedCb)(lookup);
          },
          [sharedCb](const DrogonDbException &e) {
              MembershipLookup lookup;
              lookup.status = LookupStatus::Error;
              lookup.error = e.base().what();
              (*sharedCb)(lookup);
          }
        );
    }
    catch (const std::exception &e)
    {
        MembershipLookup lookup;
        lookup.status = LookupStatus::Error;
        lookup.error = e.what();
        (*sharedCb)(lookup);
    }
    catch (...)
    {
        MembershipLookup lookup;
        lookup.status = LookupStatus::Error;
        lookup.error = "membership lookup: Mapper construction failed (unknown exception)";
        (*sharedCb)(lookup);
    }
}

void ClientOwnersRepository::resolveOwnerLabel(const std::string &clientId, OwnerLabelCallback &&cb)
{
    auto sharedCb = std::make_shared<OwnerLabelCallback>(std::move(cb));
    try
    {
        Mapper<Oauth2ClientOwners> ownersMapper(dbClient_);
        ownersMapper.findOne(
          Criteria(Oauth2ClientOwners::Cols::_client_id, CompareOperator::EQ, clientId),
          [db = dbClient_, sharedCb](const Oauth2ClientOwners &owner) {
              if (owner.getOrgId() != nullptr)
              {
                  const int32_t orgId = *owner.getOrgId();
                  try
                  {
                      Mapper<Organizations> orgsMapper(db);
                      orgsMapper.findOne(
                        Criteria(Organizations::Cols::_id, CompareOperator::EQ, orgId),
                        [sharedCb](const Organizations &org) {
                            (*sharedCb)(org.getValueOfName());
                        },
                        [sharedCb](const DrogonDbException &) {
                            (*sharedCb)("");
                        }
                      );
                  }
                  catch (...)
                  {
                      (*sharedCb)("");
                  }
                  return;
              }
              // Personal app: creator's display_name (username fallback).
              try
              {
                  Mapper<Users> usersMapper(db);
                  usersMapper.findOne(
                    Criteria(Users::Cols::_id, CompareOperator::EQ, owner.getValueOfCreatorUserId()),
                    [sharedCb](const Users &user) {
                        std::string label = user.getValueOfDisplayName();
                        if (label.empty())
                            label = user.getValueOfUsername();
                        (*sharedCb)(label);
                    },
                    [sharedCb](const DrogonDbException &) {
                        (*sharedCb)("");
                    }
                  );
              }
              catch (...)
              {
                  (*sharedCb)("");
              }
          },
          [sharedCb](const DrogonDbException &) {
              // No owners row: admin-seeded client.
              (*sharedCb)("");
          }
        );
    }
    catch (...)
    {
        // Mapper construction failure degrades exactly like a lookup
        // failure (the caller's memory-mode catch handled getDbClient()).
        (*sharedCb)("");
    }
}

}  // namespace fulla::storage::postgres
