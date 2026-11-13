#!/usr/bin/env python3
"""Validate exact patches and run the patched posix_spawn in a host model.

Usage: python3 -B tools/test-musl-aegis.py references/musl-1.2.5.tar.gz
No downloads and no Linux toolchain required. The source archive must match the
builder's pinned SHA256. This does not replace an Aegis VM regression run.
"""
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

TOOLS = Path(__file__).resolve().parent
PATCH = runpy.run_path(str(TOOLS / "patch-musl-aegis.py"))


def check_builder(root, archive):
    """Run the actual shell recipe, mocking only Linux configure/make/file."""
    workspace = root / "builder"
    for name in ("tools", "references", "mockbin", "build/musl-dynamic/usr/lib"):
        (workspace / name).mkdir(parents=True, exist_ok=True)
    for name in ("build-musl.sh", "patch-musl-aegis.py"):
        shutil.copyfile(TOOLS / name, workspace / "tools" / name)
    tarball = workspace / "references/musl-1.2.5.tar.gz"
    shutil.copyfile(archive, tarball)
    # A legacy cached libc without the recipe stamp must NOT be accepted.
    libc = workspace / "build/musl-dynamic/usr/lib/libc.so"
    libc.write_text("legacy libc\n")
    scripts = {
        "file": '#!/bin/sh\necho "$1: ELF x86-64"\n',
        "nproc": '#!/bin/sh\necho 2\n',
        "tar": f"""#!/bin/sh
{shlex.quote(shutil.which("tar"))} "$@"
cat > build/musl-aegis-source/musl-1.2.5/configure <<'SH'
#!/bin/sh
echo 'ARCH = x86_64' > config.mak
SH
""",
        "make": """#!/bin/sh
set -eu
if [ "$1" = install ]; then
    dest="${2#DESTDIR=}"
    mkdir -p "$dest/usr/lib" "$dest/usr/bin"
    echo rebuilt > "$dest/usr/lib/libc.so"
    # Pre-fixed specs avoid GNU sed requirements in this Darwin host test.
    echo "$dest/usr/lib $dest/usr/include" > "$dest/usr/lib/musl-gcc.specs"
fi
""",
    }
    for name, contents in scripts.items():
        path = workspace / "mockbin" / name
        path.write_text(contents)
        path.chmod(0o755)
    env = dict(os.environ, PATH=str(workspace / "mockbin") + ":" + os.environ["PATH"])

    def build(success=True):
        result = subprocess.run(["bash", "tools/build-musl.sh"], cwd=workspace,
                                env=env, text=True, capture_output=True, timeout=30)
        if (result.returncode == 0) != success:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout + result.stderr

    assert "Building..." in build()
    assert "skipping." in build()
    source = workspace / "build/musl-aegis-source/musl-1.2.5/src/process/posix_spawn.c"
    source.write_text("stale source which must not survive a recipe change\n")
    helper = workspace / "tools/patch-musl-aegis.py"
    helper.write_text(helper.read_text() + "\n# simulated patch revision\n")
    assert "Building..." in build()
    assert source.read_bytes() == (root / "src/process/posix_spawn.c").read_bytes()
    assert "skipping." in build()
    # A failed archive check must not replace the last valid installation.
    helper.write_text(helper.read_text() + "\n# another revision\n")
    tarball.write_bytes(b"not a musl release")
    assert "SHA256 mismatch" in build(success=False)
    assert libc.read_text() == "rebuilt\n"
    print("musl builder: legacy cache/patch invalidation/fresh source/checksum PASS")


def main(archive):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != PATCH["TARBALL_SHA256"]:
        raise ValueError("not the pinned musl 1.2.5 tarball")
    with tempfile.TemporaryDirectory(prefix="musl-aegis-test-") as directory:
        root = Path(directory)
        with tarfile.open(archive) as tar:
            for name, _, _, _ in PATCH["FILES"]:
                dest = root / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(tar.extractfile("musl-1.2.5/" + name).read())
            spawn_header = tar.extractfile("musl-1.2.5/include/spawn.h").read().decode()
            fdop = tar.extractfile("musl-1.2.5/src/process/fdop.h").read()

        pristine = {name: (root / name).read_bytes() for name, *_ in PATCH["FILES"]}
        PATCH["patch_tree"](root)
        patched = {name: (root / name).read_bytes() for name in pristine}
        PATCH["patch_tree"](root)
        assert patched == {name: (root / name).read_bytes() for name in pristine}
        # A mismatch must reject the entire patch, not partially patch spawn.
        for name, data in pristine.items():
            (root / name).write_bytes(data)
        resolver = root / "src/network/res_msend.c"
        resolver.write_bytes(resolver.read_bytes() + b"\n/* unexpected edit */\n")
        try:
            PATCH["patch_tree"](root)
        except ValueError:
            pass
        else:
            raise AssertionError("unexpected upstream source was accepted")
        assert (root / "src/process/posix_spawn.c").read_bytes() == pristine[
            "src/process/posix_spawn.c"]
        for name, data in patched.items():
            (root / name).write_bytes(data)

        # Use musl's actual spawn ABI, substituting host basic type definitions.
        start = spawn_header.index("#include <features.h>")
        end = spawn_header.index("#include <bits/alltypes.h>") + len(
            "#include <bits/alltypes.h>")
        spawn_header = (spawn_header[:start] +
                        "#include <signal.h>\n#include <sys/types.h>" +
                        spawn_header[end:])
        (root / "spawn.h").write_text(spawn_header)
        (root / "fdop.h").write_bytes(fdop)
        (root / "syscall.h").write_text("""
#define SYS_dup2 1
#define SYS_close 2
#define SYS_fcntl 3
#define SYS_setsid 4
#define SYS_setpgid 5
#define SYS_setgid 6
#define SYS_setuid 7
#define SYS_getgid 8
#define SYS_getuid 9
#define SYS_chdir 10
#define SYS_fchdir 11
#define __syscall test_syscall
#define __sys_open test_open
""")
        (root / "lock.h").write_text("""
static volatile int __abort_lock[1];
#define LOCK(x) test_lock(x)
#define UNLOCK(x) test_unlock(x)
""")
        (root / "pthread_impl.h").write_text("""
#include <limits.h>
#ifndef _NSIG
#define _NSIG NSIG
#endif
#ifndef CLONE_VM
#define CLONE_VM 0x100
#define CLONE_VFORK 0x4000
#endif
static sigset_t test_all_signals;
#define SIGALL_SET (&test_all_signals)
#define __clone test_clone
#define __get_handler_set test_handlers
#define __libc_sigaction test_sigaction
""")
        (root / "posix_spawn.c").write_bytes(patched["src/process/posix_spawn.c"])
        executable = root / "test"
        cmd = shlex.split(os.environ.get("CC", "cc"))
        # Upstream deliberately uses i-32<3U to test musl's internal signals.
        # musl 1.2.5 targets C99; use GNU C11 for this _Noreturn host model,
        # not the host compiler's default (GCC 15+ defaults to C23, which
        # changes the meaning of upstream's old-style function pointer).
        cmd += ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                "-I" + str(root),
                str(TOOLS / "musl-spawn-test.c"), "-o", str(executable)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
        check_builder(root, archive)
        print(f"{PATCH['VERSION']}: exact-source/idempotence/rejection PASS")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
