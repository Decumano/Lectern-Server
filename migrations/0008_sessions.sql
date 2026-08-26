-- Session storage. The Rust server used tower-sessions with its own SQLite
-- store; this port keeps sessions in a table it owns so the schema is
-- explicit and the C++ side has no framework-private table to match.
--
-- Drogon's built-in sessions are process-memory only, which would sign every
-- user out on restart and break any multi-process deployment — so session.cpp
-- implements cookie sessions against this table instead.
--
-- `id` is 32 bytes of CSPRNG output, hex-encoded; the cookie carries it
-- alongside an HMAC-SHA256 tag when SESSION_SECRET is set (see session.cpp).
CREATE TABLE sessions (
    id         TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL REFERENCES users(id),
    expires_at INTEGER NOT NULL
);

CREATE INDEX idx_sessions_expiry ON sessions(expires_at);
