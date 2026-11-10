#!/usr/bin/env python3
"""Exact-source musl 1.2.5 Aegis fixes; never apply a fuzzy patch.

Only the pinned upstream files (or this helper's exact output) are accepted.
Validate every input before writing any file. Existing libc users linked
statically must be rebuilt/relinked separately; replacing libc.so cannot fix them.
"""
import hashlib
from pathlib import Path
import sys

VERSION = "musl-1.2.5-aegis-spawn-1"
TARBALL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("patch context does not occur exactly once")
    return text.replace(old, new, 1)


def patch_spawn(text):
    text = replace_once(text, "\tint p[2];", "\tint err;")
    text = replace_once(text, "\tint p = args->p[1];\n", "")
    text = replace_once(text, "\tclose(args->p[0]);\n\n", "")
    start = text.index("\t\t\t/* It's possible that a file operation")
    end = text.index("\t\t\tswitch(op->cmd)", start)
    text = text[:start] + text[end:]
    text = replace_once(text, """\t\t\t\tif (fd == p) {
\t\t\t\t\tret = -EBADF;
\t\t\t\t\tgoto fail;
\t\t\t\t}
""", "")
    start = text.index("\t/* Close-on-exec flag may have been lost")
    end = text.index("\tpthread_sigmask(SIG_SETMASK", start)
    text = text[:start] + text[end:]
    text = replace_once(text, """\t/* Since sizeof errno < PIPE_BUF, the write is atomic. */
\tret = -ret;
\tif (ret) {
\t\tint r;
\t\tdo r = __syscall(SYS_write, p, &ret, sizeof ret);
\t\twhile (r<0 && r!=-EPIPE);
\t}
""", """\t/* Aegis independent children cannot write a status pipe. CLONE_VM
\t * shares args until exec detaches the VM or this child becomes a zombie;
\t * CLONE_VFORK keeps the parent suspended until then. Publish only a
\t * pre-exec error, never infer failure from a target's exit status. */
\t__atomic_store_n(&args->err, -ret, __ATOMIC_RELEASE);
""")
    text = replace_once(text, "\targs.path = path;", "\targs.err = 0;\n\targs.path = path;")
    text = replace_once(text, """\t/* The lock guards both against seeing a SIGABRT disposition change
\t * by abort and against leaking the pipe fd to fork-without-exec. */
""", """\t/* Guard against seeing a SIGABRT disposition change by abort. */
""")
    text = replace_once(text, """\tif (pipe2(args.p, O_CLOEXEC)) {
\t\tUNLOCK(__abort_lock);
\t\tec = errno;
\t\tgoto fail;
\t}

""", "")
    text = replace_once(text, "\tclose(args.p[1]);\n", "")
    text = replace_once(text, """\t\tif (read(args.p[0], &ec, sizeof ec) != sizeof ec) ec = 0;
\t\telse waitpid(pid, &(int){0}, 0);
""", """\t\tec = __atomic_load_n(&args.err, __ATOMIC_ACQUIRE);
\t\tif (ec) {
\t\t\t/* A reported pre-exec failure is ours to reap, even on EINTR. */
\t\t\twhile (waitpid(pid, &(int){0}, 0) < 0 && errno == EINTR);
\t\t}
""")
    text = replace_once(text, "\tclose(args.p[0]);\n\n", "")
    text = replace_once(text, "\nfail:\n\tpthread_sigmask", "\n\tpthread_sigmask")
    return text


# Preimage hashes are from the checksummed upstream 1.2.5 release tarball.
# Postimage hashes also make repeated invocations safe without accepting edits.
FILES = (
    ("src/process/posix_spawn.c",
     "d223cd70ee0bc2c5b8b1004cf9878ff9e4eba8b7f26187b8cda8770d1f375fc9",
     "977447a09bad3cc5bb6f2c8a14c682e2a4487103801d5bf32b1492b39b255128", patch_spawn),
    ("src/network/res_msend.c",
     "0744d65d85b8dd1efbe3703b6cfb3fd88ccfd8efe9371d4823c84576d3c8db16",
     "7e048434248488f39254c1aace39a4569433b862979854b370f70283ca38b9c9",
     lambda text: replace_once(text, "CLOCK_REALTIME", "CLOCK_MONOTONIC")),
)


def patch_tree(root):
    pending = []
    for name, before, after, transform in FILES:
        path = root / name
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest == after:
            continue
        if digest != before:
            raise ValueError(f"{path}: unexpected source SHA256 {digest}; "
                             "expected pristine musl 1.2.5 or exact Aegis output")
        output = transform(data.decode("utf-8")).encode("utf-8")
        if hashlib.sha256(output).hexdigest() != after:
            raise ValueError(f"{path}: unexpected patch output SHA256")
        pending.append((path, output))
    for path, output in pending:
        path.write_bytes(output)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} MUSL_SOURCE_DIRECTORY")
    try:
        patch_tree(Path(sys.argv[1]))
    except (OSError, ValueError) as error:
        sys.exit(f"{VERSION}: {error}")
