# Changelog fragments

Every PR that would touch `CHANGELOG.md`'s `[Unreleased]` section instead adds
one uniquely named file here: `<issue-or-pr>-<slug>.md`, containing exactly the
bullet entry (starting with `- **`) it would have prepended. Unique filenames
cannot merge-conflict, which is the whole point.

`tools/fold_changelog.py` folds all fragments into `CHANGELOG.md`'s
`[Unreleased]` section (newest first by file mtime) and deletes them; it runs
periodically, typically when merges land.
