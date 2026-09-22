#include <fulla/drogon/utils/ConsentCsrfSlots.h>

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
    std::string nonce;
    int64_t ts = 0;
};

// Process-wide mutex shared by mint() and consume() (see the header comment
// for why the lock must cover the full read-modify-write on both sides).
// NOTE (PR #157 review NIT 12): this single mutex serializes consent-slot
// traffic across ALL sessions in the process. That is fine at consent-endpoint
// volumes (human-paced form submissions); it is NOT a design that scales to
// machine-rate traffic -- if this ever shows up in profiles, shard by session
// id before anything else.
std::mutex &slotsMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<Slot> parseSlots(const ::drogon::SessionPtr &session)
{
    std::vector<Slot> slots;
    if (!session || !session->find(ConsentCsrfSlots::kSessionKey))
    {
        return slots;
    }
    const std::string raw = session->get<std::string>(ConsentCsrfSlots::kSessionKey);
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(raw);
    if (!Json::parseFromStream(reader, stream, &root, &errors) || !root.isArray())
    {
        // Corrupt payload -> treat as no live slots (fail-closed, same as an
        // expired nonce; the next authorize mints a fresh one).
        return slots;
    }
    for (const auto &item : root)
    {
        if (item.isObject() && item.isMember("n") && item.isMember("t"))
        {
            slots.push_back({item["n"].asString(), item["t"].asInt64()});
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
        item["n"] = slot.nonce;
        item["t"] = slot.ts;
        root.append(item);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    // Session::insert uses std::map::insert semantics: an EXISTING key is
    // silently NOT overwritten. Every rewrite of the slot list must therefore
    // erase first, or mints/consumes after the first would be no-ops.
    session->erase(ConsentCsrfSlots::kSessionKey);
    session->insert(
      ConsentCsrfSlots::kSessionKey, Json::writeString(writer, root)
    );
}

}  // namespace

void ConsentCsrfSlots::mint(
  const ::drogon::SessionPtr &session,
  const std::string &nonce,
  int64_t nowSeconds
)
{
    if (!session)
    {
        LOG_INFO << "[ConsentCsrfSlots] mint: no session";
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
    while (slots.size() >= kMaxSlots)
    {
        slots.erase(slots.begin());  // oldest first (mint order is append order)
    }
    slots.push_back({nonce, nowSeconds});
    writeSlots(session, slots);
}

bool ConsentCsrfSlots::consume(
  const ::drogon::SessionPtr &session,
  const std::string &nonce,
  int64_t nowSeconds
)
{
    if (!session || nonce.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(slotsMutex());
    auto slots = parseSlots(session);
    for (auto it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->nonce == nonce && (nowSeconds - it->ts) <= kTtlSeconds)
        {
            slots.erase(it);  // one-shot: only the matching slot is consumed
            writeSlots(session, slots);
            return true;
        }
    }
    return false;
}

bool ConsentCsrfSlots::peek(
  const ::drogon::SessionPtr &session,
  const std::string &nonce,
  int64_t nowSeconds
)
{
    if (!session || nonce.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(slotsMutex());
    // Read-only: the lock still matters (parseSlots reads the session value
    // a concurrent consume() may be mid-rewrite on).
    const auto slots = parseSlots(session);
    for (const auto &slot : slots)
    {
        if (slot.nonce == nonce && (nowSeconds - slot.ts) <= kTtlSeconds)
        {
            return true;
        }
    }
    return false;
}

}  // namespace fulla::drogon::utils
