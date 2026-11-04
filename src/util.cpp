#include "util.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lectern::util {

void fInitCrypto()
{
    if (sodium_init() < 0)
    {
        throw std::runtime_error("libsodium failed to initialise");
    }
}

int64_t fNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

int64_t fNowS()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch())
        .count();
}

namespace {

std::string fToHex(const unsigned char *data, size_t len)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

}  // namespace

std::string fUuidV4()
{
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    // Version 4, variant 1 — the bit patterns RFC 4122 requires.
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    const std::string hex = fToHex(bytes.data(), bytes.size());
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
           "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

std::string fRandomHex(size_t count)
{
    std::vector<unsigned char> bytes(count);
    randombytes_buf(bytes.data(), bytes.size());
    return fToHex(bytes.data(), bytes.size());
}

std::string fBase64Encode(const std::vector<unsigned char> &data)
{
    return fBase64Encode(std::string_view(
        reinterpret_cast<const char *>(data.data()), data.size()));
}

std::string fBase64Encode(std::string_view data)
{
    // sodium_base64_ENCODED_LEN includes room for the NUL terminator.
    const size_t out_len =
        sodium_base64_ENCODED_LEN(data.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(out_len, '\0');
    sodium_bin2base64(out.data(),
                      out_len,
                      reinterpret_cast<const unsigned char *>(data.data()),
                      data.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    // Drop the trailing NUL that sodium_bin2base64 writes.
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::string> fBase64Decode(std::string_view data)
{
    std::vector<unsigned char> out(data.size());  // decoding only shrinks
    size_t decoded_len = 0;
    const int rc = sodium_base642bin(out.data(),
                                     out.size(),
                                     data.data(),
                                     data.size(),
                                     nullptr,
                                     &decoded_len,
                                     nullptr,
                                     sodium_base64_VARIANT_ORIGINAL);
    if (rc != 0)
    {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char *>(out.data()),
                       decoded_len);
}

std::string fSha256Hex(std::string_view data)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
                       reinterpret_cast<const unsigned char *>(data.data()),
                       data.size());
    return fToHex(digest.data(), digest.size());
}

std::string fSha256File(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return {};
    }

    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);

    std::array<char, 64 * 1024> buffer{};
    while (in)
    {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
        {
            crypto_hash_sha256_update(
                &state,
                reinterpret_cast<const unsigned char *>(buffer.data()),
                static_cast<unsigned long long>(got));
        }
    }
    if (in.bad())
    {
        return {};
    }

    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    return fToHex(digest.data(), digest.size());
}

bool fConstantTimeEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string fHashPassword(std::string_view password)
{
    std::array<char, crypto_pwhash_STRBYTES> out{};
    if (crypto_pwhash_str(out.data(),
                          password.data(),
                          password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        // Only happens when the machine is out of memory for the KDF.
        throw std::runtime_error("password hashing failed");
    }
    return std::string(out.data());
}

bool fVerifyPassword(std::string_view password, const std::string &phc)
{
    if (phc.empty() || phc.size() >= crypto_pwhash_STRBYTES)
    {
        return false;
    }
    return crypto_pwhash_str_verify(
               phc.c_str(), password.data(), password.size()) == 0;
}

std::string fTrim(std::string_view s)
{
    const auto fIsSpace = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    size_t begin = 0;
    while (begin < s.size() && fIsSpace(static_cast<unsigned char>(s[begin])))
    {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && fIsSpace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

std::string fToLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool fStartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool fEndsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> fSplit(std::string_view s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        const size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos)
        {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string_view fBasenameOf(std::string_view rel_path)
{
    const size_t pos = rel_path.rfind('/');
    return pos == std::string_view::npos ? rel_path : rel_path.substr(pos + 1);
}

std::string fExtensionOf(std::string_view name)
{
    const std::string_view base = fBasenameOf(name);
    const size_t pos = base.rfind('.');
    // A leading dot is a hidden file, not an extension (".mdp" has none),
    // which is what Rust's Path::extension() also reports.
    if (pos == std::string_view::npos || pos == 0)
    {
        return {};
    }
    return fToLower(base.substr(pos + 1));
}

namespace {

void fSetEnv(const std::string &name, const std::string &value)
{
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 0 /* never overwrite */);
#endif
}

}  // namespace

void fLoadDotenv(const fs::path &path)
{
    std::ifstream in(path);
    if (!in)
    {
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        const std::string trimmed = fTrim(line);
        if (trimmed.empty() || trimmed.front() == '#')
        {
            continue;
        }

        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }

        std::string name = fTrim(trimmed.substr(0, equals));
        std::string value = fTrim(trimmed.substr(equals + 1));
        if (name.empty())
        {
            continue;
        }

        // Strip one layer of matching quotes, the way dotenv files are
        // conventionally written.
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        // A value already in the environment wins; on Windows _putenv_s has
        // no no-overwrite mode, so check first.
        if (std::getenv(name.c_str()) == nullptr)
        {
            fSetEnv(name, value);
        }
    }
}

std::optional<std::string> fEnv(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
}

std::string fEnvOr(const char *name, std::string fallback)
{
    return fEnv(name).value_or(std::move(fallback));
}

int64_t fEnvI64(const char *name, int64_t fallback)
{
    const auto value = fEnv(name);
    if (!value || value->empty())
    {
        return fallback;
    }
    try
    {
        return std::stoll(*value);
    }
    catch (const std::exception &)
    {
        return fallback;
    }
}

bool fEnvBool(const char *name, bool fallback)
{
    const auto value = fEnv(name);
    if (!value)
    {
        return fallback;
    }
    const std::string lowered = fToLower(*value);
    return lowered == "1" || lowered == "true";
}

std::optional<std::string> fReadFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
    {
        return std::nullopt;
    }
    return buffer.str();
}

std::optional<std::vector<unsigned char>> fReadFileBytes(const fs::path &path)
{
    const auto text = fReadFile(path);
    if (!text)
    {
        return std::nullopt;
    }
    return std::vector<unsigned char>(text->begin(), text->end());
}

void fWriteFileAtomic(const fs::path &path, std::string_view content)
{
    const fs::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    // A unique sibling, so two concurrent writers can't clobber each other's
    // temp file and produce a torn document.
    const fs::path temp =
        parent / (path.filename().string() + "." + fRandomHex(8) + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            throw fs::filesystem_error(
                "cannot open temp file for writing",
                temp,
                std::make_error_code(std::errc::permission_denied));
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            std::error_code ec;
            fs::remove(temp, ec);
            throw fs::filesystem_error(
                "write failed",
                temp,
                std::make_error_code(std::errc::io_error));
        }
    }

    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec)
    {
        // Windows refuses to rename onto an existing file; fall back to a
        // remove-then-rename, which is the best this platform offers.
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            throw fs::filesystem_error("rename failed", temp, path, ec);
        }
    }
}

}  // namespace lectern::util
