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
void init_crypto();

// ── Time ──

int64_t now_ms();
int64_t now_s();

// ── Ids and encodings ──

/// Random UUIDv4 in canonical 8-4-4-4-12 hex form, from libsodium's CSPRNG.
std::string uuid_v4();

/// `count` bytes of CSPRNG output, hex-encoded (so 2*count characters).
std::string random_hex(size_t count);

std::string base64_encode(const std::vector<unsigned char> &data);
std::string base64_encode(std::string_view data);

/// Decodes standard base64. Returns nullopt on anything malformed — used for
/// the PDF bytes CDP hands back, which must never be half-decoded.
std::optional<std::string> base64_decode(std::string_view data);

/// Lowercase hex SHA-256 of the bytes.
std::string sha256_hex(std::string_view data);

/// Lowercase hex SHA-256 of a file's contents; empty string if unreadable,
/// matching the Rust `hash_file`'s behaviour of degrading rather than failing.
std::string sha256_file(const std::filesystem::path &path);

/// Constant-time equality, for comparing MACs and tokens.
bool constant_time_equals(std::string_view a, std::string_view b);

// ── Passwords (Argon2id via libsodium) ──

/// PHC-format Argon2id hash string. Verifiable by, and compatible with,
/// hashes written by the Rust server's `argon2` crate.
std::string hash_password(std::string_view password);
bool verify_password(std::string_view password, const std::string &phc);

// ── Strings ──

std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char sep);

/// Everything after the last '/', or the whole string when there is none.
std::string_view basename_of(std::string_view rel_path);

/// Lowercased extension without the dot; empty when there is none.
std::string extension_of(std::string_view name);

// ── Environment ──

/// Reads `KEY=value` lines from a `.env` file into the process environment,
/// standing in for the Rust build's `dotenvy`. Existing environment variables
/// win, so a real deployment's config can't be overridden by a stray file.
/// Missing file is not an error.
void load_dotenv(const std::filesystem::path &path = ".env");

std::string env_or(const char *name, std::string fallback);
std::optional<std::string> env(const char *name);
int64_t env_i64(const char *name, int64_t fallback);
bool env_bool(const char *name, bool fallback);

// ── Files ──

std::optional<std::string> read_file(const std::filesystem::path &path);
std::optional<std::vector<unsigned char>> read_file_bytes(
    const std::filesystem::path &path);

/// Writes via a temp file in the same directory plus a rename, so a crashed
/// or concurrent write can't leave a half-written document on disk. Creates
/// parent directories. Throws std::filesystem::filesystem_error on failure.
void write_file_atomic(const std::filesystem::path &path,
                       std::string_view content);

}  // namespace lectern::util
