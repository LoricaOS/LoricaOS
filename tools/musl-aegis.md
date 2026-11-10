# musl 1.2.5 Aegis launch migration

Patch/helper version: `musl-1.2.5-aegis-spawn-1`.

`build-musl.sh` verifies the musl **1.2.5** release tarball against SHA256
`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`.
`patch-musl-aegis.py` accepts only exact upstream file hashes or its own exact
patched output. Unexpected source fails; it does not fall back to a fuzzy
patch. The helper also retains the existing resolver `CLOCK_MONOTONIC` fix.

## Contract

Aegis independent fork/vfork children have empty capability tables before exec.
They can duplicate/close inherited descriptors and exec, but cannot open files
or write a status pipe. Vigil therefore opens quiet-mode `/dev/null` in the
authorized parent, checks child dup/close operations, and closes its temporary
descriptor in the parent on success or fork failure.

Upstream musl 1.2.5 `posix_spawn` retries the error-pipe write indefinitely when
the child gets `ENOCAP`, while the `CLONE_VFORK` parent is suspended. The Aegis
patch instead initializes a shared error field to zero and release-stores a
positive errno before `_exit(127)` on pre-exec failure. The parent acquire-loads
the field after `__clone` returns, reaps reported failures (retrying `EINTR`),
and publishes the pid only for success. Clone errors remain `-pid`.

This **Aegis-specific** recipe relies on the kernel suspending the parent until
the child's shared VM is detached or the child is a zombie, and on exec
detaching the VM before waking the parent. It preserves file-action order,
signal handling, cancellation state, and the abort lock. No capability is
granted to the child. `posix_spawn_file_actions_addopen` can register an action
but the spawn honestly returns `ENOCAP` (`EPERM` in the current ABI) when that
pre-exec open is attempted. Unsupported file actions are not emulated in the
parent, which would change ordering and authority semantics.

A successfully exec'd program exiting 127, or a death after exec's point of no
return, is a **child outcome**, not a spawn error. Neither is inferred from
`waitpid` status.

## Rebuild scope

The build stamp hashes the builder and complete patch helper. Legacy cached
libc installations with no stamp, and installations from any changed recipe,
are rebuilt from a freshly extracted verified archive. Stale source and objects
are not reused. The stamp is written only after installation and wrapper fixups.

**Replacing `libc.so` does not fix statically linked programs.** Static libc
users, prebuilt component artifacts, and independently built toolchains must be
rebuilt/relinked with this fix separately. This patch does not silently migrate
other builders (including `build-arm64-userland.sh`). Dynamic executables must
actually load the rebuilt guest libc; merely recompiling a test against an
unrelated host libc does not test it.

## Focused validation

From the repository root:

```sh
sh tools/test-vigil-launch.sh
sh tools/test-vigil-diagnostics.sh
python3 -B tools/test-musl-aegis.py references/musl-1.2.5.tar.gz
cc -fsyntax-only -Wall -Wextra -Werror user/bin/vforkstress/main.c
bash -n tools/build-musl.sh
```

The musl host model compiles the actual patched source with mock clone/exec and
strict child syscalls. It covers exact source/idempotence/rejection, clone
failure, `EBADF`, `ENOENT`, `ENOCAP`, ordered actions on caller-owned descriptors,
target exit 127, post-PONR death, error-child reaping with `EINTR`, restored
cancellation/signal masks and abort-lock release. It also executes the real
builder with configure/make/file mocked to test cache invalidation and checksum
failure without needing a Linux toolchain. These are deterministic **host
models**, not proof of the kernel's VM/scheduling behavior.

For actual integration, build on Linux with `bash tools/build-musl.sh`, rebuild
Vigil and `vforkstress`, assemble the normal image with the rebuilt libc, and
boot Aegis with `vfork_stress`. The executable performs 400 successful spawns
and 400 exact `EBADF` failures, plus missing-executable, unprivileged addopen and
self-exec exit-127 checks. Every case checks for no remaining children. Only
the verified full run emits `[VFORKSTRESS] OK 400/400`; a hang must be caught by
the external VM timeout. Validate normal quiet boot/service launch too.
