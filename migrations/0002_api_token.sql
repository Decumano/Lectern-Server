ALTER TABLE users ADD COLUMN api_token TEXT NOT NULL DEFAULT '';

-- Partial index: the default '' is shared by every not-yet-migrated user, so a
-- plain UNIQUE index would reject the migration itself once more than one row
-- exists. Only real (non-empty) tokens need to be unique.
CREATE UNIQUE INDEX idx_users_api_token ON users(api_token) WHERE api_token != '';
