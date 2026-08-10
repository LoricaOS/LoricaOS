# tools/fuzz — host fuzzing for the network-sourced parsers

herald downloads bytes and parses them: a `.hpkg` ustar archive, the manifest
inside it, and the repository's `Release` / `Packages`. The DHCP client parses a
server reply that carries no authentication at all. All four are `(buf, len)`
entry points with no syscall dependencies, so they fuzz on the host directly.

Needs `clang` with libFuzzer (not the musl cross toolchain herald ships with).

```sh
make
make seeds                       # writes corpus_* with valid inputs
./fuzz_tar corpus_tar -max_len=16384
```

A crash is written to `./crash-*`; replay with `./fuzz_tar ./crash-<hash>`.

| target | entry point |
|---|---|
| `fuzz_tar` | `tar_extract_mem` + `tar_find_mem` — ustar parsing **and** path containment |
| `fuzz_manifest` | `manifest_parse` + the bare-name invariants the installer relies on |
| `fuzz_repo` | `release_find_hash`, `parse_stanza`, `herald_version_gt` |
| `fuzz_dhcp` | the DHCP option parser |

## These assert security properties, not just memory safety

A pure memory-safety fuzz of a path handler misses the bug that matters, so the
harnesses check the invariant directly:

- **`fuzz_tar`** compiles `tar.c` with `-Dopen=fz_open -Dmkdir=fz_mkdir …`, so
  extraction performs no real filesystem I/O and every path `tar.c` *would*
  create is asserted to live under `dest_root` with no traversal component.
  A failure here is a directory escape, not a crash.
- **`fuzz_manifest`** asserts that `id`, `exec` and each `caps.<binary>` come
  back as bare names — they become `/apps/<id>/` and `/etc/aegis/caps.d/<exec>`,
  so a value that is not a real name escapes the directory it names.
- **`fuzz_repo`** asserts every stanza field is NUL-terminated inside its own
  array and that the stanza walk always advances.

## Findings

**2026-08-09** — `fuzz_manifest` found that `manifest_parse` accepted `.` and
`..` for `id`/`exec`/`caps.<binary>` (only `/` was rejected), so `id=..` made
`herald remove ..` run `unlink("/apps/../<exec>")`. `fuzz_repo` found signed
overflow in `herald_version_gt`, which inverts the anti-rollback guard. Both
fixed; `user/bin/herald/make selftest` is the in-tree regression for them.

`fuzz_tar` ran 40M executions and `fuzz_dhcp` 107M with no finding.

## Coverage limits

- `fuzz_dhcp` covers option parsing only — not the socket/state machine around
  it, and not the packet the client sends.
- `fuzz_repo` stubs the crypto and network symbols, so it exercises the
  *parsers* below the trust chain, never the signature check itself.
- `fuzz_repo` does not cover `sources_load`, which reads a root-owned config
  file rather than anything network-sourced.
- Neither the herald install driver (`main.c`) nor `db.c` is fuzzed; the
  manifest values they consume are, up to the point they leave the parser.
