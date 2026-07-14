-- Comments can now anchor to a location in the file: a text range (docs), a
-- cell range + tab (spreadsheets), or a specific item (glossary entries,
-- beasts, ...). Stored as a small client-defined JSON blob; NULL means the
-- comment is about the file as a whole. Comments are also no longer limited
-- to Documents (enforcement lives in src/share.rs).
ALTER TABLE comments ADD COLUMN anchor TEXT;
