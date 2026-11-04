// Small helpers shared across the server: hashing, ids, base64, time and the
// string/path handling every handler needs. Everything crypto-flavoured goes
// through libsodium rather than hand-rolled code.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lectern::util {

/// Initialises libsodium. Must be called once before anything else here;
/// throws std::runtime_error if the library refuses to start.
void fInitCrypto();

// ── Time ──

int64_t fNowMs();
int64_t fNowS();

// ── Ids and encodings ──

/// Random UUIDv4 in canonical 8-4-4-4-12 hex form, from libsodium's CSPRNG.
std::string fUuidV4();

/// `count` bytes of CSPRNG output, hex-encoded (so 2*count characters).
std::string fRandomHex(size_t count);

std::string fBase64Encode(const std::vector<unsigned char> &data);
std::string fBase64Encode(std::string_view data);

/// Decodes standard base64. Returns nullopt on anything malformed — used for
/// the PDF bytes CDP hands back, which must never be half-decoded.
std::optional<std::string> fBase64Decode(std::string_view data);

/// Lowercase hex SHA-256 of the bytes.
std::string fSha256Hex(std::string_view data);

/// Lowercase hex SHA-256 of a file's contents; empty string if unreadable,
/// matching the Rust `hash_file`'s behaviour of degrading rather than failing.
std::string fSha256File(const std::filesystem::path &path);

/// Constant-time equality, for comparing MACs and tokens.
bool fConstantTimeEquals(std::string_view a, std::string_view b);

// ── Passwords (Argon2id via libsodium) ──

/// PHC-format Argon2id hash string. Verifiable by, and compatible with,
/// hashes written by the Rust server's `argon2` crate.
std::string fHashPassword(std::string_view password);
bool fVerifyPassword(std::string_view password, const std::string &phc);

// ── Strings ──

std::string fTrim(std::string_view s);
std::string fToLower(std::string_view s);
bool fStartsWith(std::string_view s, std::string_view prefix);
bool fEndsWith(std::string_view s, std::string_view suffix);
std::vector<std::string> fSplit(std::string_view s, char sep);

/// Everything after the last '/', or the whole string when there is none.
std::string_view fBasenameOf(std::string_view rel_path);

/// Lowercased extension without the dot; empty when there is none.
std::string fExtensionOf(std::string_view name);

// ── Environment ──

/// Reads `KEY=value` lines from a `.env` file into the process environment,
/// standing in for the Rust build's `dotenvy`. Existing environment variables
/// win, so a real deployment's config can't be overridden by a stray file.
/// Missing file is not an error.
void fLoadDotenv(const std::filesystem::path &path = ".env");

std::string fEnvOr(const char *name, std::string fallback);
std::optional<std::string> fEnv(const char *name);
int64_t fEnvI64(const char *name, int64_t fallback);
bool fEnvBool(const char *name, bool fallback);

// ── Files ──

std::optional<std::string> fReadFile(const std::filesystem::path &path);
std::optional<std::vector<unsigned char>> fReadFileBytes(
    const std::filesystem::path &path);

/// Writes via a temp file in the same directory plus a rename, so a crashed
/// or concurrent write can't leave a half-written document on disk. Creates
/// parent directories. Throws std::filesystem::filesystem_error on failure.
void fWriteFileAtomic(const std::filesystem::path &path,
                       std::string_view content);

}  // namespace lectern::util
