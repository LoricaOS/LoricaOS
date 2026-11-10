# Independent-child shell regressions

Run from the repository root:

```sh
python3 user/bin/stsh/tests/host.py
```

Or run `make -C user/bin/stsh host-test`. The suite compiles temporary host
executables with `cc` (`CC` can override it); it does not execute Aegis syscall
numbers on the host. The real capability UX gets a syntax check, not an
authentication test.

The host shim marks old fork images as capless and rejects their
`open`/`read`/`write`/`fopen` calls. A successful exec creates an independent
image. The `caps` test builtin also rejects execution in the old fork image.
Failure injection covers fork/pipe/dup2 failures, interrupted/short/zero writes,
and pipeline cleanup. Repeated pipelines run under a 16-descriptor host limit.
This is regression evidence, **not a replacement for kernel policy tests**.

## Rules under test

- The authorized parent expands and prepares redirects in lexical order,
  including every repeated open and its side effects. A failure prevents later
  redirect expansion/open effects. Descriptor duplication snapshots its source
  at that point: `2>&1 >out` differs from `>out 2>&1`.
- External, foreground, background, builtin, and function commands share that
  preparation rule. Parent descriptors are restored after builtin errors.
  Saved descriptors are strictly above 2; originally closed standard
  descriptors are restored by closing them, not by aliasing another saved fd.
  `command` retains its explicit function bypass through preparation and
  bootstrap, including inside same-name function wrappers.
- Heredoc bodies remain literal. Their opening-line suffixes (redirects,
  pipelines, background markers) are not discarded. Multiple documents retain
  order. At most 4000 body bytes can be prefetched into an Aegis pipe; larger
  documents fail explicitly instead of silently truncating or using a tempfile
  or capless writer. Short/interrupted writes are handled.
- Subshells, substitutions, and builtin/function pipeline/background stages
  exec a fresh `/bin/stsh` before interpreting anything. Their supported local
  variables, function tokens, `$0`, positional parameters (including empty and
  quoted values), and previous status survive. Parent state stays independent.
  stsh currently stores all variables in one environment store; this migration
  preserves that existing behavior, not a new exported/local distinction.
- A real host PTY verifies that Ctrl-C interrupts interactive subshells,
  captures, nested state jobs, and captures inside stdin-redirected functions
  without killing the shell. Status 130 and terminal ownership restoration are
  checked after each interruption. A private close-on-exec controlling
  descriptor keeps job control independent of temporary stdin redirects;
  fork children explicitly close it before exec.
- Child pre-exec failures use status 125 (setup) or 127 (exec), never diagnostic
  writes. The parent reports these statuses, including when reaping background
  jobs. Ordinary commands can return the same statuses, so diagnostics explicitly
  do not pretend to distinguish them from bootstrap/exec failure.

## State bootstrap v1

`/bin/stsh --state-v1 PAYLOAD` accepts a NUL-free, decimal-length-prefixed
record stream. It is data, never generated shell source:

1. `STSH1` string and previous status (0..255).
2. Variable count and `KEY=VALUE` strings (existing `ENV_MAX`, 255 bytes each).
3. `$0`, parameter count, and parameter strings (existing `MAX_PARAMS`).
4. Function count (64 max), each function name (63 bytes max) and raw token
   stream (existing `MAX_TOKENS` bound).
5. Program token stream.

Each token carries a validated enum and length-prefixed raw word text; nonword
tokens require empty text. Counts, string lengths, duplicate/empty function
names, enums, version, and complete payload consumption are validated in a dry
pass before state is applied or interpreted. Allocations are bounded by the
same schema. The encoded payload including its terminating NUL is at most
65536 bytes; overflow is an explicit error, not partial state.

There is no filesystem handoff, inherited capability/admin cache, grant marker,
or “next fork” mechanism. Anyone can supply bootstrap data, but receives only
the ordinary fresh stsh policy subject to the kernel's caller ceiling.

## Required integration checks

The parent integration run must rebuild the actual LoricaOS userland and boot
an image using the matching independent-child kernel:

- Exercise these cases with real `/bin/stsh`, not an external `/bin/sh`.
- Confirm redirected commands, heredocs, subshells, substitutions, function and
  builtin pipelines work with genuinely capless fork children.
- Confirm interactive pipelines share a foreground group, terminal ownership
  is restored after completion, and Ctrl-C works through descriptor-scoped
  `tcsetpgrp`/`TIOCSPGRP` under baseline IPC-read. Noninteractive/fresh
  interpreters remain in their caller's job group.
- Confirm failed downstream setup terminates/reaps earlier pipeline stages
  using the parent's existing signal authority, with no widened policy.
- Check `admin herald ...` invokes the independently authenticated,
  fixed-allowlist `login --admin-exec /bin/herald -- ...` flow. Neither the
  shell prompt nor the command claims an inherited administrative session.
- Check the in-tree minimal fallback shell separately if that image builds it.

Some images fetch a static external `/bin/sh` artifact. Editing stsh and the
in-tree fallback does **not** rebuild or migrate that artifact's libc, fork,
redirection, substitution, or session behavior. It needs its own upstream
migration/rebuild; do not compensate with inherited authority or kernel grants.
