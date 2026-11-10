#!/usr/bin/env python3
"""Exercise the common rootfs factory; --real additionally requires e2fsprogs.

Default tests run the whole production builder with a stateful debugfs mock.
They do not emulate Aegis protection: the guest denial expectation is in docs.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
BUILDER = ROOT / "tools/build-rootfs.sh"
ANCHORS = ("/bin", "/sbin", "/apps")
parser = argparse.ArgumentParser()
parser.add_argument("--real", action="store_true")
args = parser.parse_args()
bash = shutil.which("bash")
assert bash

MOCK = r'''#!/usr/bin/env python3
import json,os,pathlib,shlex,sys
a=sys.argv[1:]
image=pathlib.Path(a[-1])
state=pathlib.Path(str(image)+".layout.json")
if pathlib.Path(sys.argv[0]).name=="mke2fs":
    state.write_text(json.dumps({"/":"directory"}));sys.exit(0)
layout=json.loads(state.read_text())
if "-R" in a:
    path=a[a.index("-R")+1].split(" ",1)[1]
    kind=layout.get(path)
    if os.environ.get("BAD_STAT_ANCHOR")==path: kind=os.environ["BAD_STAT_TYPE"]
    if kind: print("Inode: 12   Type: "+kind+"    Mode: 0755")
    else: print("File not found by ext2_lookup")
    sys.exit(0)
for line in sys.stdin:
    p=shlex.split(line)
    if not p: continue
    command=p[0]
    if command=="mkdir":
        if os.environ.get("DROP_ANCHOR")!=p[1]: layout.setdefault(p[1],"directory")
    elif command=="write":
        layout.setdefault(p[2],"regular")
    elif command=="symlink":
        layout.setdefault(p[1],"symlink")
state.write_text(json.dumps(layout))
'''


def fixture(base):
    base.mkdir()
    (base / "rootfs/lib").mkdir(parents=True)
    (base / "rootfs-desktop/lib").mkdir(parents=True)
    (base / "payload").write_text("fixture payload\n")
    (base / "rootfs.manifest").write_text(
        "payload /bin/probe\npayload /sbin/probe\n")
    (base / "rootfs.desktop.manifest").write_text("")


def build(base, profile, tools, extra=None):
    env = dict(os.environ, AEGIS_PROFILE=profile, DEBUGFS=str(tools[0]),
               MKE2FS=str(tools[1]))
    env.update(extra or {})
    result = subprocess.run([bash, str(BUILDER), "root.img"], cwd=base,
                            env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    return result


with tempfile.TemporaryDirectory(prefix="lorica-factory-anchors-") as directory:
    tmp = Path(directory)
    tools = tmp / "tools"
    tools.mkdir()
    for name in ("debugfs", "mke2fs"):
        tool = tools / name
        tool.write_text(MOCK)
        tool.chmod(0o755)
    mocked = (tools / "debugfs", tools / "mke2fs")
    for profile in ("server", "desktop"):
        base = tmp / profile
        fixture(base)
        result = build(base, profile, mocked)
        assert result.returncode == 0, result.stdout
        layout = json.loads((base / "root.img.layout.json").read_text())
        assert all(layout.get(a) == "directory" for a in ANCHORS), layout
        assert not any(p.startswith("/apps/") for p in layout), layout
    print("factory: server + desktop precreate all builtin anchors without GUI payload")

    base = tmp / "existing-directories"
    fixture(base)
    for skeleton in ("rootfs", "rootfs-desktop"):
        for anchor in ANCHORS:
            (base / (skeleton + anchor)).mkdir()
    result = build(base, "desktop", mocked)
    assert result.returncode == 0, result.stdout
    print("factory: existing real anchor directories in base/overlay remain accepted")

    # All skeleton sources participating in a profile must reject files and
    # symlinks, including dangling links, before the output image is created.
    cases = 0
    for profile, skeleton in (("server", "rootfs"), ("desktop", "rootfs"),
                              ("desktop", "rootfs-desktop")):
        for anchor in ANCHORS:
            for kind in ("file", "symlink", "dangling"):
                base = tmp / ("invalid-" + str(cases))
                fixture(base)
                entry = base / (skeleton + anchor)
                if kind == "file":
                    entry.write_text("not a directory")
                else:
                    entry.symlink_to("lib" if kind == "symlink" else "absent")
                result = build(base, profile, mocked)
                assert result.returncode != 0, result.stdout
                assert "granting anchor must be a real directory" in result.stdout
                assert not (base / "root.img").exists()
                cases += 1
    print(f"factory: {cases} file/symlink/dangling skeleton conflicts rejected before assembly")

    # debugfs success is not proof of a directory. Test final-image validation
    # against a missing inode and both disallowed inode types.
    for kind in ("missing", "regular", "symlink"):
        base = tmp / ("bad-image-" + kind)
        fixture(base)
        extra = ({"DROP_ANCHOR": "/apps"} if kind == "missing" else
                 {"BAD_STAT_ANCHOR": "/apps", "BAD_STAT_TYPE": kind})
        result = build(base, "server", mocked, extra)
        assert result.returncode != 0, result.stdout
        assert "missing/non-directory granting anchor in image: /apps" in result.stdout
    print("factory: final image rejects missing, ordinary-file, and symlink anchor inodes")

    if args.real:
        debugfs = os.environ.get("DEBUGFS") or shutil.which("debugfs")
        mke2fs = os.environ.get("MKE2FS") or shutil.which("mke2fs")
        if not debugfs or not mke2fs:
            raise SystemExit("--real requires debugfs and mke2fs (or DEBUGFS/MKE2FS)")
        for profile in ("server", "desktop"):
            base = tmp / ("real-" + profile)
            fixture(base)
            result = build(base, profile, (debugfs, mke2fs))
            assert result.returncode == 0, result.stdout
            # Match the kernel's documented zero-extension before inspection.
            with (base / "root.img").open("r+b") as image:
                image.truncate(90112 * 512)
            for anchor in ANCHORS:
                result = subprocess.check_output(
                    [debugfs, "-R", "stat " + anchor, str(base / "root.img")],
                    stderr=subprocess.STDOUT, text=True)
                assert "Type: directory" in result, result
            apps = subprocess.check_output(
                [debugfs, "-R", "ls -p /apps", str(base / "root.img")],
                stderr=subprocess.STDOUT, text=True)
            assert "gui-installer" not in apps, apps
        print("factory: real ext2 server + desktop anchor layouts verified")

print("factory anchor tests: PASS; guest INSTALL-denial check remains separate")
