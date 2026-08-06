-- Deletion tombstones: when a workspace file is deleted (web UI delete, or a
-- sync client's remote_delete), remember that it was deleted and when. Sync
-- clients that still hold a local copy the server has never told them about
-- (lost .officesuite-sync state, a machine that was offline, a restored
-- backup) used to re-upload such files as if they were new, resurrecting
-- them on every other device. With the tombstone they can tell "deleted
-- elsewhere" apart from "created here" and honor the deletion instead.
-- A tombstone is cleared the moment the path is written again, so genuinely
-- re-created files sync normally.
CREATE TABLE deleted_files (
    user_id    TEXT NOT NULL REFERENCES users(id),
    rel_path   TEXT NOT NULL,
    deleted_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, rel_path)
);
