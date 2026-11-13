# ARM64 baseline checkpoint — 2026-11-13

Branch: `checkpoint/apple-silicon-20261113` on
`https://github.com/loricaos/loricaos`, based on
`b302c30e5cef02d7e7860d84e1745385df3abcce`.

This is a work-in-progress source backup, not a release or a main-branch
landing. It preserves the session's existing ARM64 userland/server baseline:

- apply the existing Aegis musl spawn/DNS patches to the ARM64 build;
- verify the upstream tarball and invalidate cached sysroots by recipe hash;
- make server-build checkout/tool paths configurable and build only the
  needed account-tool libinstall subset;
- run the musl host model under explicit GNU C11;
- provide a headless QEMU runner and account/login/external-command test.

See `arm64-qemu.md` for build/run instructions and the native Apple boundary.
The companion Aegis checkpoint uses the same branch name on
`https://github.com/loricaos/aegis`.

## Validation status

During the preceding session, the static-musl ARM64 server image built and
booted under QEMU, with account creation, authenticated stsh, `uname -m`,
version-file reading and `/bin` listing passing. The musl source/host model
checks also passed. Those full image/boot checks were not rerun merely to
publish this checkpoint; do not confuse the Aegis native diagnostic passes
with a native LoricaOS rootfs boot.

The ARM64 kernel reached first native EL0/isolation on J514s/T6030 in the
companion Aegis work. The full init ABI, trustworthy boot entropy,
explicit RAM-rootfs ownership and native init execution remain follow-ups.

## Backup limits

Ignored sysroots, rootfs images, ISOs and logs remain local under `build/`;
they are not included in this source push. Preserve needed artifacts in a
separate owner-approved backup with hashes. At each session boundary, commit
and push a checkpoint branch, verify its remote commit, and record both
repositories' commit IDs. Do not infer permission to land on `main`.
