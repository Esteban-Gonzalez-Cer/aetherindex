# Evidence archive

Each dated folder holds the results a report in [`docs/reports`](..) cites: test result
logs, CSV/JSON measurements and run metadata.

Before the repository was published, the archive was reduced:

- Build and configure logs, `CMakeCache.txt` files, assembly dumps and superseded
  intermediate runs were removed from version control. They remain in the Git history.
- Absolute paths of the development machine were replaced with `<repo>` (the repository
  root) and `~` (the home directory).

Because of the second change, SHA-256 values recorded in the manifest files
(`manifest.json`, `artifact_sha256.json`, `source-sha256.json` and similar) no longer match
files whose paths were rewritten, and some manifests list files that are no longer tracked.
