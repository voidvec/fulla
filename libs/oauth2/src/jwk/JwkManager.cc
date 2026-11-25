#include <fulla/common/config/ConfigManager.h>
#include <fulla/oauth2/jwk/JwkManager.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <cstring>

namespace fulla::oauth2
{

void JwkManager::log(fulla::common::ports::LogLevel level, const std::string &message) const
{
    if (logger_)
    {
        logger_->log(level, message);
    }
}

JwkManager::~JwkManager()
{
    for (auto &entry : keys_)
    {
        if (entry.pkey)
        {
            EVP_PKEY_free(static_cast<EVP_PKEY *>(entry.pkey));
            entry.pkey = nullptr;
        }
    }
}

const JwkManager::KeyEntry *JwkManager::findEntry(const std::string &kid) const
{
    for (const auto &entry : keys_)
    {
        if (entry.kid == kid)
            return &entry;
    }
    return nullptr;
}

const JwkManager::KeyEntry *JwkManager::activeEntry() const
{
    return findEntry(activeKid_);
}

bool JwkManager::init(const Json::Value &config)
{
    if (initialized_)
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: init() called more than once; ignoring "
          "(init-once-then-read-only contract). The existing keys are kept."
        );
        return true;
    }

    // #110-B (decision B): the multi-key keystore directory is the richest
    // source and takes precedence -- when configured it is authoritative
    // (mixing it with the single-key env vars below would make the effective
    // key set depend on ambient env, which a rotation runbook cannot
    // tolerate). A configured-but-broken directory is a HARD failure, never
    // a silent fallthrough to a different key source.
    const std::string keystoreDir = config.get("signing_keystore_dir", "").asString();
    if (!keystoreDir.empty())
    {
        if (loadKeystoreDir(keystoreDir))
        {
            initialized_ = true;
            log(
              fulla::common::ports::LogLevel::Info,
              "JwkManager: Loaded " + std::to_string(keys_.size()) +
                " signing key(s) from keystore dir " + keystoreDir + " (active kid: " +
                activeKid_ + ")"
            );
            return true;
        }
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: signing_keystore_dir=" + keystoreDir +
            " is configured but failed to load -- refusing to fall back to "
            "another key source (fix the directory and restart)"
        );
        return false;
    }

    const char *keyEnv = fulla::common::config::ConfigManager::getProcessEnv("FULLA_SIGNING_KEY");
    if (keyEnv && std::strlen(keyEnv) > 0)
    {
        KeyEntry entry;
        entry.kid = config.get("kid", "key-1").asString();
        if (loadPemInto(entry, keyEnv))
        {
            keys_.push_back(std::move(entry));
            activeKid_ = keys_.front().kid;
            initialized_ = true;
            log(
              fulla::common::ports::LogLevel::Info,
              "JwkManager: Loaded signing key from FULLA_SIGNING_KEY env"
            );
            return true;
        }
    }

    const char *keyPathEnv = fulla::common::config::ConfigManager::getProcessEnv("FULLA_JWT_KEY_PATH");
    if (keyPathEnv && std::strlen(keyPathEnv) > 0)
    {
        std::ifstream file(keyPathEnv);
        if (file.is_open())
        {
            std::stringstream ss;
            ss << file.rdbuf();
            KeyEntry entry;
            entry.kid = config.get("kid", "key-1").asString();
            if (loadPemInto(entry, ss.str()))
            {
                keys_.push_back(std::move(entry));
                activeKid_ = keys_.front().kid;
                initialized_ = true;
                log(
                  fulla::common::ports::LogLevel::Info,
                  "JwkManager: Loaded signing key from FULLA_JWT_KEY_PATH=" +
                    std::string(keyPathEnv)
                );
                return true;
            }
        }
        log(
          fulla::common::ports::LogLevel::Warn,
          "JwkManager: Failed to load key from FULLA_JWT_KEY_PATH=" + std::string(keyPathEnv)
        );
    }

    std::string keyPath = config.get("signing_key_path", "").asString();
    if (!keyPath.empty())
    {
        std::ifstream file(keyPath);
        if (file.is_open())
        {
            std::stringstream ss;
            ss << file.rdbuf();
            KeyEntry entry;
            entry.kid = config.get("kid", "key-1").asString();
            if (loadPemInto(entry, ss.str()))
            {
                keys_.push_back(std::move(entry));
                activeKid_ = keys_.front().kid;
                initialized_ = true;
                log(
                  fulla::common::ports::LogLevel::Info,
                  "JwkManager: Loaded signing key from " + keyPath
                );
                return true;
            }
        }
        log(
          fulla::common::ports::LogLevel::Warn, "JwkManager: Failed to load key from " + keyPath
        );
    }

    // #102: the ephemeral fallback is DEV ONLY. In production a per-boot
    // random key silently invalidates every outstanding token on restart and
    // publishes a JWKS kid nothing can resolve -- refuse instead (defense in
    // depth: ConfigManager::validate() gates this earlier; this catches any
    // path that bypasses validation). init() false makes the caller abort.
    {
        // Read through ConfigManager so .env and the process environment are
        // decided the same way here as in the server assembly: a FULLA_ENV
        // set only in .env used to be missed by this very check.
        const char *fullaEnv = fulla::common::config::ConfigManager::getEnv("FULLA_ENV");
        if (fullaEnv && std::string(fullaEnv) == "production")
        {
            log(
              fulla::common::ports::LogLevel::Error,
              "JwkManager: no signing key configured and FULLA_ENV=production -- "
              "the ephemeral fallback is DEV ONLY. Configure signing_keystore_dir, "
              "FULLA_SIGNING_KEY, FULLA_JWT_KEY_PATH, or "
              "plugins.OAuth2Plugin.config.oidc.signing_key_path and restart"
            );
            return false;
        }
    }

    log(
      fulla::common::ports::LogLevel::Warn,
      "JwkManager: No signing key configured, generating ephemeral key (DEV ONLY)"
    );
    {
        KeyEntry entry;
        // #87: honor a configured kid on the ephemeral path too (previously
        // hardcoded, so tests/ops needing a distinct kid had to go through
        // env+PEM). The ephemeral marker stays the default.
        entry.kid = config.get("kid", "ephemeral-dev-key").asString();
        if (generateEphemeralKey(entry))
        {
            // generateEphemeralKey() hands its EVP_PKEY to the pending entry
            // (see its #110-B note).
            keys_.push_back(std::move(entry));
            activeKid_ = keys_.front().kid;
            initialized_ = true;
            return true;
        }
    }

    log(fulla::common::ports::LogLevel::Error, "JwkManager: Failed to initialize");
    return false;
}

bool JwkManager::loadPemInto(KeyEntry &entry, const std::string &pemData)
{
    BIO *bio = BIO_new_mem_buf(pemData.c_str(), static_cast<int>(pemData.length()));
    if (!bio)
        return false;

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!pkey)
    {
        log(
          fulla::common::ports::LogLevel::Error, "JwkManager: Failed to parse PEM private key"
        );
        return false;
    }

    // PR #176 review MINOR-1: strict RS256 means RSA keys ONLY. A dropped-in
    // EC/Ed25519 PEM would otherwise load, silently vanish from the JWKS
    // (RSA-only component extraction) and — as the active key — sign ECDSA
    // signatures under an alg=RS256 header no RP can verify. Wrong key type
    // is a hard init failure, same as a parse failure.
    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA)
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: signing key is not an RSA key (strict RS256) -- refusing to load"
        );
        EVP_PKEY_free(pkey);
        return false;
    }
    // P2-3 audit fix: reject weak RSA keys — a 512/1024-bit PEM would
    // otherwise be accepted for RS256 signing.
    if (EVP_PKEY_bits(pkey) < 2048)
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: RSA signing key is shorter than 2048 bits -- refusing to load"
        );
        EVP_PKEY_free(pkey);
        return false;
    }

    entry.pkey = pkey;
    return true;
}

bool JwkManager::loadKeystoreDir(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec))
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: keystore dir " + dir + " is not a directory"
        );
        return false;
    }

    // Collect <kid>.pem candidates (sorted for deterministic load/JWKS order).
    // PR #176 review NIT: case-insensitive suffix match so a hand-named
    // <kid>.PEM (Windows habit) is not silently skipped; the kid itself keeps
    // the filename's case. Convention: lowercase .pem extension.
    std::vector<std::pair<std::string, std::string>> kids;  // (kid, actual filename)
    for (const auto &item : fs::directory_iterator(fs::path(dir), ec))
    {
        if (!item.is_regular_file())
            continue;
        const std::string filename = item.path().filename().string();
        if (filename.size() <= 4)
            continue;
        std::string suffix = filename.substr(filename.size() - 4);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
        if (suffix != ".pem")
            continue;
        kids.emplace_back(filename.substr(0, filename.size() - 4), filename);
    }
    std::sort(kids.begin(), kids.end());
    if (kids.empty())
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: keystore dir " + dir + " contains no <kid>.pem files"
        );
        return false;
    }

    // The active kid marker is a one-line text file.
    std::ifstream activeFile(fs::path(dir) / "active_kid");
    if (!activeFile.is_open())
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: keystore dir " + dir + " has no active_kid marker file"
        );
        return false;
    }
    std::string activeKid;
    std::getline(activeFile, activeKid);
    // Trim CR/whitespace on BOTH ends for hand-edited markers (convention:
    // the kid starts at column 0; leading whitespace would otherwise silently
    // mismatch -- PR #176 review NIT).
    const auto isSpace = [](char c) {
        return c == '\r' || c == ' ' || c == '\t';
    };
    while (!activeKid.empty() && isSpace(activeKid.back()))
        activeKid.pop_back();
    while (!activeKid.empty() && isSpace(activeKid.front()))
        activeKid.erase(activeKid.begin());

    std::vector<KeyEntry> loaded;
    for (const auto &[kid, actualName] : kids)
    {
        if (kid.empty())
            continue;
        // Open the ACTUAL filename (the suffix check is case-insensitive, so
        // it may be .PEM; on a case-sensitive FS opening kid+".pem" would
        // miss it).
        std::ifstream pemFile(fs::path(dir) / actualName);
        // PR #176 review MINOR-2: an unopenable .pem is a hard failure, not a
        // skip -- silently dropping the NEW key would no-op rotation step 1
        // ("Publish") and only surface when step 2 flips active_kid.
        if (!pemFile.is_open())
        {
            for (auto &e : loaded)
                EVP_PKEY_free(static_cast<EVP_PKEY *>(e.pkey));
            log(
              fulla::common::ports::LogLevel::Error,
              "JwkManager: keystore " + dir + "/" + actualName + " cannot be opened"
            );
            return false;
        }
        std::stringstream ss;
        ss << pemFile.rdbuf();
        KeyEntry entry;
        entry.kid = kid;
        if (!loadPemInto(entry, ss.str()))
        {
            for (auto &e : loaded)
                EVP_PKEY_free(static_cast<EVP_PKEY *>(e.pkey));
            log(
              fulla::common::ports::LogLevel::Error,
              "JwkManager: keystore " + dir + "/" + kid + ".pem failed to parse"
            );
            return false;
        }
        loaded.push_back(std::move(entry));
    }
    if (loaded.empty())
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: keystore dir " + dir + " loaded zero usable keys"
        );
        return false;
    }
    bool activeFound = false;
    for (const auto &entry : loaded)
    {
        if (entry.kid == activeKid)
        {
            activeFound = true;
            break;
        }
    }
    if (!activeFound)
    {
        for (auto &e : loaded)
            EVP_PKEY_free(static_cast<EVP_PKEY *>(e.pkey));
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: keystore active_kid marker names [" + activeKid +
            "] but no matching .pem exists in " + dir
        );
        return false;
    }

    keys_ = std::move(loaded);
    activeKid_ = activeKid;
    return true;
}

bool JwkManager::generateEphemeralKey(KeyEntry &entry)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx)
        return false;

    if (EVP_PKEY_keygen_init(ctx) <= 0)
    {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0)
    {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
    {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY_CTX_free(ctx);
    entry.pkey = pkey;
    return true;
}

namespace
{
// See JwkManager.h's top comment: byte-for-byte identical algorithm to
// fulla::drogon::adapters::OpenSslCryptoProvider::base64UrlEncode /
// fulla::common::testing::FakeCryptoProvider::base64UrlEncode
// (deliberately duplicated in each, for dependency-direction reasons).
constexpr char kBase64UrlAlphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64UrlEncodeImpl(const unsigned char *bytes, size_t length)
{
    std::string out;
    out.reserve((length + 2) / 3 * 4);

    size_t i = 0;
    while (i + 3 <= length)
    {
        const uint32_t n = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                           static_cast<uint32_t>(bytes[i + 2]);
        out.push_back(kBase64UrlAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[n & 0x3F]);
        i += 3;
    }

    const size_t remaining = length - i;
    if (remaining == 1)
    {
        const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
        out.push_back(kBase64UrlAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(n >> 12) & 0x3F]);
    }
    else if (remaining == 2)
    {
        const uint32_t n =
          (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kBase64UrlAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(n >> 6) & 0x3F]);
    }

    return out;
}
}  // namespace

std::string JwkManager::base64UrlEncode(const unsigned char *data, size_t len)
{
    return base64UrlEncodeImpl(data, len);
}

std::string JwkManager::base64UrlEncode(const std::string &data)
{
    return base64UrlEncodeImpl(reinterpret_cast<const unsigned char *>(data.data()), data.size());
}

std::string JwkManager::signJwt(const Json::Value &payload) const
{
    const KeyEntry *active = activeEntry();
    if (!initialized_ || !active)
    {
        log(
          fulla::common::ports::LogLevel::Error,
          "JwkManager: Cannot sign JWT - not initialized (or active kid missing)"
        );
        return "";
    }

    Json::Value header;
    header["alg"] = "RS256";
    header["typ"] = "JWT";
    // #110-B: the active kid routes verifiers (and JWKS consumers) to the
    // intended public key.
    header["kid"] = active->kid;

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "";
    std::string headerJson = Json::writeString(writerBuilder, header);
    std::string payloadJson = Json::writeString(writerBuilder, payload);

    std::string headerB64 = base64UrlEncode(headerJson);
    std::string payloadB64 = base64UrlEncode(payloadJson);

    std::string signingInput = headerB64 + "." + payloadB64;

    EVP_MD_CTX *mdCtx = EVP_MD_CTX_new();
    if (!mdCtx)
        return "";

    EVP_PKEY *pkey = static_cast<EVP_PKEY *>(active->pkey);

    if (EVP_DigestSignInit(mdCtx, nullptr, EVP_sha256(), nullptr, pkey) <= 0)
    {
        EVP_MD_CTX_free(mdCtx);
        return "";
    }

    if (EVP_DigestSignUpdate(mdCtx, signingInput.c_str(), signingInput.length()) <= 0)
    {
        EVP_MD_CTX_free(mdCtx);
        return "";
    }

    size_t sigLen = 0;
    if (EVP_DigestSignFinal(mdCtx, nullptr, &sigLen) <= 0)
    {
        EVP_MD_CTX_free(mdCtx);
        return "";
    }

    std::vector<unsigned char> signature(sigLen);
    if (EVP_DigestSignFinal(mdCtx, signature.data(), &sigLen) <= 0)
    {
        EVP_MD_CTX_free(mdCtx);
        return "";
    }

    EVP_MD_CTX_free(mdCtx);

    std::string sigB64 = base64UrlEncode(signature.data(), sigLen);

    return signingInput + "." + sigB64;
}

namespace
{
// base64url reverse lookup: value 0..63, -1 for anything else ('=' is
// illegal in the unpadded compact-JWT form this decoder serves).
int b64UrlValue(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}
}  // namespace

bool JwkManager::base64UrlDecode(const std::string &input, std::string &output)
{
    output.clear();
    const size_t remainder = input.size() % 4;
    if (remainder == 1)
        return false;  // 6*len+1 bits cannot align to bytes
    output.reserve((input.size() / 4) * 3 + (remainder == 0 ? 0 : remainder - 1));

    // Decodes one group of `count` (2..4) symbols, emitting count-1 bytes.
    auto decodeGroup = [&output](const char *group, size_t count) -> bool {
        int v[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < count; i++)
        {
            v[i] = b64UrlValue(group[i]);
            if (v[i] < 0)
                return false;
        }
        // #87 L1: reject non-canonical trailing zero bits (RFC 4648 §3.5 --
        // the unpadded base64url form a JWT mandates). "AB" carries 12 bits
        // of which only 8 are meaning-bearing, so the low 4 bits of the last
        // symbol must be zero; "ABC" carries 18 bits for 16, low 2 bits zero.
        // The signature still pins exact bytes, so this is strictness, not a
        // vulnerability -- but a strict decoder must not bless them.
        if (count == 2 && (v[1] & 0x0F) != 0)
            return false;
        if (count == 3 && (v[2] & 0x03) != 0)
            return false;
        const unsigned int triple = (static_cast<unsigned int>(v[0]) << 18) |
                                    (static_cast<unsigned int>(v[1]) << 12) |
                                    (static_cast<unsigned int>(v[2]) << 6) |
                                    static_cast<unsigned int>(v[3]);
        output += static_cast<char>((triple >> 16) & 0xFF);
        if (count > 2)
            output += static_cast<char>((triple >> 8) & 0xFF);
        if (count > 3)
            output += static_cast<char>(triple & 0xFF);
        return true;
    };

    size_t pos = 0;
    for (size_t group = 0; group < input.size() / 4; ++group, pos += 4)
    {
        if (!decodeGroup(input.data() + pos, 4))
        {
            output.clear();
            return false;
        }
    }
    if (remainder != 0 && !decodeGroup(input.data() + pos, remainder))
    {
        output.clear();
        return false;
    }
    return true;
}

JwkManager::JwtVerificationResult JwkManager::verifyJwt(
  const std::string &jwt,
  const std::string &expectedIssuer,
  long long nowSecs,
  const std::string &expectedAudience
) const
{
    return verifyCore(jwt, expectedIssuer, nowSecs, expectedAudience, nullptr);
}

std::optional<Json::Value> JwkManager::verifyAndDecode(
  const std::string &jwt,
  const std::string &expectedIssuer,
  long long nowSecs,
  const std::string &expectedAudience,
  JwtVerificationResult *rejectionReason
) const
{
    Json::Value payload;
    const auto result = verifyCore(jwt, expectedIssuer, nowSecs, expectedAudience, &payload);
    if (rejectionReason != nullptr)
        *rejectionReason = result;
    if (result != JwtVerificationResult::Ok)
        return std::nullopt;
    return payload;
}

JwkManager::JwtVerificationResult JwkManager::verifyCore(
  const std::string &jwt,
  const std::string &expectedIssuer,
  long long nowSecs,
  const std::string &expectedAudience,
  Json::Value *verifiedPayloadOut
) const
{
    if (!initialized_ || keys_.empty())
    {
        log(
          fulla::common::ports::LogLevel::Error, "JwkManager::verifyJwt: not initialized"
        );
        return JwtVerificationResult::NotInitialized;
    }

    // Structure: exactly two dots, three non-empty segments.
    const size_t firstDot = jwt.find('.');
    if (firstDot == std::string::npos || firstDot == 0)
        return JwtVerificationResult::Malformed;
    const size_t secondDot = jwt.find('.', firstDot + 1);
    if (secondDot == std::string::npos || secondDot == firstDot + 1 ||
        secondDot + 1 == jwt.size() || jwt.find('.', secondDot + 1) != std::string::npos)
        return JwtVerificationResult::Malformed;

    const std::string headerB64 = jwt.substr(0, firstDot);
    const std::string payloadB64 = jwt.substr(firstDot + 1, secondDot - firstDot - 1);
    const std::string signatureB64 = jwt.substr(secondDot + 1);

    // Header policy (enforced before any trust is placed in claims): strict
    // RS256 -- "none"/HS256 must never verify.
    std::string headerJson;
    if (!base64UrlDecode(headerB64, headerJson))
        return JwtVerificationResult::Malformed;
    Json::Value header;
    {
        Json::CharReaderBuilder builder;
        std::istringstream iss(headerJson);
        if (!Json::parseFromStream(builder, iss, &header, nullptr))
            return JwtVerificationResult::Malformed;
    }
    if (!header.isMember("alg") || !header["alg"].isString() ||
        header["alg"].asString() != "RS256")
        return JwtVerificationResult::BadAlg;
    // #110-B: route by kid across ALL loaded keys. A kid naming no loaded
    // key is a hard KidMismatch (previously: != the single configured kid).
    // An absent kid (pre-kid-era tokens) tries every loaded key in order --
    // strictly no narrower than the old single-key behavior.
    const KeyEntry *entry = nullptr;
    if (header.isMember("kid") && header["kid"].isString() && !header["kid"].asString().empty())
    {
        entry = findEntry(header["kid"].asString());
        if (!entry)
            return JwtVerificationResult::KidMismatch;
    }

    // RS256 signature over the exact header.payload segments as received.
    const std::string signingInput = headerB64 + "." + payloadB64;
    std::string signature;
    if (!base64UrlDecode(signatureB64, signature))
        return JwtVerificationResult::Malformed;
    bool signatureOk = false;
    if (entry != nullptr)
    {
        signatureOk = verifyRs256Signature(entry->pkey, signingInput, signature);
    }
    else
    {
        // Absent kid: try every loaded key (RFC 7515 §4.1.4 permits this when
        // the verifier holds several keys).
        for (const auto &candidate : keys_)
        {
            if (verifyRs256Signature(candidate.pkey, signingInput, signature))
            {
                signatureOk = true;
                break;
            }
        }
    }
    if (!signatureOk)
        return JwtVerificationResult::BadSignature;

    // Claims (trustworthy now that the signature verified).
    std::string payloadJson;
    if (!base64UrlDecode(payloadB64, payloadJson))
        return JwtVerificationResult::Malformed;
    Json::Value payload;
    {
        Json::CharReaderBuilder builder;
        std::istringstream iss(payloadJson);
        if (!Json::parseFromStream(builder, iss, &payload, nullptr))
            return JwtVerificationResult::Malformed;
    }
    if (!payload.isMember("iss") || !payload["iss"].isString() ||
        payload["iss"].asString() != expectedIssuer)
        return JwtVerificationResult::IssuerMismatch;
    if (!payload.isMember("exp") || !payload["exp"].isNumeric() ||
        payload["exp"].asInt64() <= nowSecs)
        return JwtVerificationResult::Expired;
    // #87 M2: nbf is optional, but when present it must be a numeric claim
    // with nbf <= nowSecs (RFC 7519 §4.1.5; no leeway, mirroring exp above).
    // A non-numeric nbf cannot be evaluated -- fail closed as Malformed.
    if (payload.isMember("nbf"))
    {
        if (!payload["nbf"].isNumeric())
            return JwtVerificationResult::Malformed;
        if (payload["nbf"].asInt64() > nowSecs)
            return JwtVerificationResult::NotYetValid;
    }
    if (!payload.isMember("sub") || !payload["sub"].isString() ||
        payload["sub"].asString().empty())
        return JwtVerificationResult::MissingSubject;
    // #87 M1: optional audience pinning. aud may be a string or an array of
    // strings (RFC 7519 §4.1.3); the expected audience must appear in it.
    if (!expectedAudience.empty())
    {
        bool audOk = false;
        if (payload["aud"].isString())
            audOk = payload["aud"].asString() == expectedAudience;
        else if (payload["aud"].isArray())
        {
            for (const auto &element : payload["aud"])
            {
                if (element.isString() && element.asString() == expectedAudience)
                {
                    audOk = true;
                    break;
                }
            }
        }
        if (!audOk)
            return JwtVerificationResult::AudienceMismatch;
    }
    if (verifiedPayloadOut != nullptr)
        *verifiedPayloadOut = payload;
    return JwtVerificationResult::Ok;
}

bool JwkManager::verifyRs256Signature(
  const void *pkeyVoid, const std::string &signingInput, const std::string &signature
)
{
    EVP_MD_CTX *mdCtx = EVP_MD_CTX_new();
    if (!mdCtx)
        return false;
    bool ok = false;
    EVP_PKEY *pkey = static_cast<EVP_PKEY *>(const_cast<void *>(pkeyVoid));
    if (EVP_DigestVerifyInit(mdCtx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(mdCtx, signingInput.data(), signingInput.size()) == 1)
    {
        const int rc = EVP_DigestVerifyFinal(
          mdCtx,
          reinterpret_cast<const unsigned char *>(signature.data()),
          signature.size()
        );
        ok = (rc == 1);
    }
    EVP_MD_CTX_free(mdCtx);
    return ok;
}

bool JwkManager::getPublicKeyComponents(const KeyEntry &entry, std::string &n, std::string &e) const
{
    if (!entry.pkey)
        return false;

    EVP_PKEY *pkey = static_cast<EVP_PKEY *>(entry.pkey);

    BIGNUM *bn_n = nullptr;
    BIGNUM *bn_e = nullptr;

    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &bn_n) <= 0 || !bn_n)
    {
        return false;
    }

    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &bn_e) <= 0 || !bn_e)
    {
        BN_free(bn_n);
        return false;
    }

    int nLen = BN_num_bytes(bn_n);
    std::vector<unsigned char> nBuf(nLen);
    BN_bn2bin(bn_n, nBuf.data());
    n = base64UrlEncode(nBuf.data(), nBuf.size());

    int eLen = BN_num_bytes(bn_e);
    std::vector<unsigned char> eBuf(eLen);
    BN_bn2bin(bn_e, eBuf.data());
    e = base64UrlEncode(eBuf.data(), eBuf.size());

    BN_free(bn_n);
    BN_free(bn_e);
    return true;
}

Json::Value JwkManager::getJwks() const
{
    Json::Value jwks;
    jwks["keys"] = Json::Value(Json::arrayValue);

    if (!initialized_)
        return jwks;

    // #110-B: publish EVERY loaded key so verifiers keep resolving tokens
    // signed by a since-deactivated (but still trusted) key during the
    // rotation grace window.
    for (const auto &entry : keys_)
    {
        std::string n, e;
        if (!getPublicKeyComponents(entry, n, e))
            continue;
        Json::Value key;
        key["kty"] = "RSA";
        key["use"] = "sig";
        key["alg"] = "RS256";
        key["kid"] = entry.kid;
        key["n"] = n;
        key["e"] = e;
        jwks["keys"].append(key);
    }

    return jwks;
}

}  // namespace fulla::oauth2
