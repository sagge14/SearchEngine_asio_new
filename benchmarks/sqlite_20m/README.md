# SQLite restore benchmark fixture

`inverted_index.sqlite` is a schema-v2 legacy rowid database intended to
measure startup restoration and the one-time migration to `WITHOUT ROWID`.

Fixture contents:

- 100,000 words
- 20,000 documents
- 20,000,000 postings
- file size: 930,086,912 bytes
- SHA-256: `12CDA90BE3B55E4ACCF4B0369A57103BE9909401E7BEBAF29ABBAFCB6D356ABE`

Regenerate from the repository root with an x86 Visual C++ environment:

```powershell
benchmarks\sqlite_20m\generate_sqlite_20m.exe `
  benchmarks\sqlite_20m\inverted_index.sqlite
```

The generator refuses to overwrite an existing database.

Validate the existing fixture:

```powershell
benchmarks\sqlite_20m\generate_sqlite_20m.exe --verify `
  benchmarks\sqlite_20m\inverted_index.sqlite
```
