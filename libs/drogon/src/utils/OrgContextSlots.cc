#include <fulla/drogon/utils/OrgContextSlots.h>

#include <json/json.h>

#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace fulla::drogon::utils
{

namespace
{

struct Slot
{
    std::string state;
    int32_t orgId = 0;
    int64_t ts = 0;
};

// Process-wide mutex shared by mint/peek/consume (same rationale as
// ConsentCsrfSlots: Drogon Session does not serialize read-modify-write
// sequences).
std::mutex &slotsMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<Slot> parseSlots(const ::drogon::SessionPtr &session)
{
    std::vector<Slot> slots;
    if (!session || !session->find(OrgContextSlots::kSessionKey))
    {
        return slots;
    }
    const std::string raw = session->get<std::string>(OrgContextSlots::kSessionKey);
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(raw);
    if (!Json::parseFromStream(reader, stream, &root, &errors) || !root.isArray())
    {
        // Corrupt payload -> treat as no live slots (fail-closed: the flow
        // degrades to a no-org-context issuance, never a wrong-org one).
        return slots;
    }
    for (const auto &item : root)
    {
        // Type guards (review nit 2): a malformed entry must degrade to
        // "no live slots" exactly as the header promises -- jsoncpp's
        // asXXX() would otherwise throw Json::LogicError out of the
        // mint/consume call sites.
        if (item.isObject() && item.isMember("s") && item.isMember("o") &&
            item.isMember("t") && item["s"].isString() && item["o"].isInt() &&
            item["t"].isInt64())
        {
            slots.push_back({item["s"].asString(), item["o"].asInt(), item["t"].asInt64()});
        }
    }
    return slots;
}

void writeSlots(const ::drogon::SessionPtr &session, const std::vector<Slot> &slots)
{
    Json::Value root(Json::arrayValue);
    for (const auto &slot : slots)
    {
        Json::Value item;
        item["s"] = slot.state;
        item["o"] = slot.orgId;
        item["t"] = (Json::Int64)slot.ts;
        root.append(item);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    // Session::insert is std::map::insert semantics (existing key silently
    // NOT overwritten) -- erase first, or rewrites after the first are
    // no-ops (the Session::insert pitfall, same as ConsentCsrfSlots).
    session->erase(OrgContextSlots::kSessionKey);
    session->insert(
      OrgContextSlots::kSessionKey, Json::writeString(writer, root)
    );
}

}  // namespace

void OrgContextSlots::mint(
  const ::drogon::SessionPtr &session,
  const std::string &state,
  int32_t orgId,
  int64_t nowSeconds
)
{
    if (!session || state.empty())
    {
        LOG_INFO << "[OrgContextSlots] mint: no session or empty state";
        return;
    }
    std::lock_guard<std::mutex> lock(slotsMutex());
    auto slots = parseSlots(session);
    // Drop expired entries first so the cap applies to live slots only.
    slots.erase(
      std::remove_if(
        slots.begin(),
        slots.end(),
        [nowSeconds](const Slot &slot) {
            return (nowSeconds - slot.ts) > kTtlSeconds;
        }
      ),
      slots.end()
    );
    // A re-authorization with the same state replaces its slot rather than
    // accumulating duplicates.
    slots.erase(
      std::remove_if(
        slots.begin(),
        slots.end(),
        [&state](const Slot &slot) { return slot.state == state; }
      ),
      slots.end()
    );
    while (slots.size() >= kMaxSlots)
    {
        slots.erase(slots.begin());  // oldest first (mint order is append order)
    }
    slots.push_back({state, orgId, nowSeconds});
    writeSlots(session, slots);
}

std::optional<int32_t> OrgContextSlots::consume(
  const ::drogon::SessionPtr &session,
  const std::string &state,
  int64_t nowSeconds
)
{
    if (!session || state.empty())
    {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(slotsMutex());
    auto slots = parseSlots(session);
    for (auto it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->state == state && (nowSeconds - it->ts) <= kTtlSeconds)
        {
            const int32_t orgId = it->orgId;
            slots.erase(it);  // one-shot: only the matching slot is consumed
            writeSlots(session, slots);
            return orgId;
        }
    }
    return std::nullopt;
}

}  // namespace fulla::drogon::utils
