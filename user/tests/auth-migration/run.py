#!/usr/bin/env python3
"""Host tests. Aegis syscalls are mocked; no host identity changes are issued."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CC = os.environ.get("CC", "cc")


def run(args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


with tempfile.TemporaryDirectory(prefix="lorica-auth-test-") as tmp:
    tmp = Path(tmp)
    # Darwin provides crypt in libc but no crypt.h; Linux provides libcrypt.
    (tmp / "crypt.h").write_text("char *crypt(const char *, const char *);\n")
    crypt_lib = [] if os.uname().sysname == "Darwin" else ["-lcrypt"]
    for name in ("auth", "home", "elevate", "login"):
        out = tmp / name
        run([CC, "-std=c11", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
             "-Wall", "-Wextra", "-I" + str(tmp),
             "-I" + str(ROOT / "user/lib/libinstall"),
             "-I" + str(ROOT / "user/lib/libauth"),
             str(HERE / (name + "_test.c")), "-o", str(out)] +
            (crypt_lib if name == "auth" else []))
        run([str(out)])

    # Compile the production dispatcher with side-effect-free operation mocks.
    # This isolates command policy from package/network/crypto implementation.
    main = (ROOT / "user/bin/herald/main.c").read_text().split(
        "int main(int argc, char **argv)\n", 1)[1]
    dispatcher = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int entered, recovered, operations, denied, recovery;
static int install_admin_enter(const char *p, int *n, char ***v, const char *pw)
{ (void)p;(void)n;(void)v;(void)pw; ++entered; return denied ? -1 : 0; }
static int txn_recover(void) { assert(entered); ++recovered; return recovery; }
static int txn_finalize(void) { assert(entered); ++operations; return 0; }
static int txn_rollback(void) { assert(entered); ++operations; return 0; }
static long syscall(long n) { (void)n; assert(entered); return 0; }
#define SYS_INSTALL_COMMIT 516
static void usage(void) {}
static int cmd_list(void) { ++operations; return 0; }
static int cmd_sync(void) { assert(entered); ++operations; return 0; }
static int cmd_upgrade(const char *s) { (void)s;assert(entered);++operations;return 0; }
static int cmd_verify(const char *s) { (void)s; ++operations; return 0; }
static int cmd_info(const char *s) { (void)s; ++operations; return 0; }
static int cmd_search(const char *s) { (void)s; ++operations; return 0; }
static int cmd_install(const char *s) { (void)s;assert(entered);++operations;return 0; }
static int cmd_install_named(const char *s) { return cmd_install(s); }
static int cmd_remove(const char *s) { return cmd_install(s); }
static int ends_with(const char *s, const char *end)
{ size_t n=strlen(s), m=strlen(end);return n>=m && !strcmp(s+n-m,end); }
int herald_main(int argc, char **argv)
''' + main + r'''
static void check(const char *command, const char *arg, int mutate)
{
    char *av[]={"herald",(char *)command,(char *)arg,NULL};
    for(int fail=0;fail<2;fail++) {
        entered=recovered=operations=0;denied=fail;recovery=mutate?0:1;
        int rc=herald_main(arg?3:2,av);
        assert(entered==mutate);
        if(mutate && fail) assert(rc==1 && !recovered && !operations);
        else assert(rc==0 && operations==1 && recovered==mutate);
    }
}
int main(void)
{
    check("list",NULL,0);check("search","foo",0);
    check("info","foo",0);check("verify","foo.hpkg",0);
    check("sync",NULL,1);check("upgrade",NULL,1);
    check("upgrade","foo",1);check("install","foo",1);
    check("install","foo.hpkg",1);check("remove","foo",1);
    const char *actions[]={"complete","rollback"};
    for(int i=0;i<2;i++) for(int fail=0;fail<2;fail++) {
        char *av[]={"herald","recover",(char *)actions[i],NULL};
        entered=recovered=operations=0;denied=fail;recovery=1;
        assert(herald_main(3,av)==fail);
        assert(entered==1 && recovered==!fail && operations==!fail);
    }
    char *invalid[]={"herald","recover","nonsense",NULL};
    entered=recovered=operations=0;denied=0;
    assert(herald_main(3,invalid)==2 && !entered && !recovered && !operations);
    puts("herald: all mutating commands/recovery authorize first; read-only never prompt or recover");
}
'''
    source = tmp / "herald_dispatch.c"
    source.write_text(dispatcher)
    out = tmp / "herald"
    run([CC, "-std=c11", "-Wall", "-Wextra", str(source), "-o", str(out)])
    run([str(out)])

    gui = (ROOT / "user/bin/gui-installer/main.c").read_text()
    targets = (ROOT / "user/lib/libinstall/admin_targets.h").read_text()
    assembly = (ROOT / "tools/assemble-desktop-rootfs.sh").read_text()
    fixed_gui_path = re.search(
        r'#define ADMIN_GUI_INSTALLER_TARGET "([^"]+)"', targets).group(1)
    deployed_gui_path = re.search(
        r'write_file "\$tmpelf" "([^"]+)" 0100755', assembly).group(1)
    assert fixed_gui_path == deployed_gui_path
    assert '"/bin/gui-installer"' not in gui
    print("GUI deployment: compiled target equals the exact assembled executable path")

    # Exercise actual production navigation functions, not a duplicate state
    # machine. The renderer/IPC and nonreturning reexec/exit are the only mocks.
    welcome = "static void handle_key_welcome(char c)\n" + gui.split(
        "static void handle_key_welcome(char c)\n", 1)[1].split(
        "\nstatic void handle_key_disk", 1)[0]
    back = "static void handle_back(void)\n" + gui.split(
        "static void handle_back(void)\n", 1)[1].split(
        "\n/* Mouse click handling.", 1)[0]
    gui_test = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "admin_targets.h"
enum { SCREEN_WELCOME=1, SCREEN_DISK, SCREEN_USER, SCREEN_CONFIRM, SCREEN_PROGRESS };
static struct {
    int screen, dirty, lfd, install_failed;
    void *lwin;
    char admin_pw[128], validation_error[128];
} g_st;
static int ready, queried, entered, destroyed, closed, exited, reexec;
static jmp_buf escape;
static int install_admin_ready(const char *path)
{ assert(!strcmp(path, ADMIN_GUI_INSTALLER_TARGET)); ++queried; return ready; }
static int install_admin_enter(const char *path, int *argc, char ***argv, const char *pw)
{
    assert(queried && destroyed==1 && closed==1);
    assert(!strcmp(path, ADMIN_GUI_INSTALLER_TARGET));
    assert(*argc==1 && !strcmp((*argv)[0],"gui-installer"));
    assert(!strcmp(pw,"secret")); ++entered;
    if (reexec) longjmp(escape,2);
    return -1;
}
static void lumen_window_destroy(void *win) { (void)win; ++destroyed; }
static int test_close(int fd) { (void)fd; ++closed; return 0; }
static void test_exit(int code) { assert(code==1); ++exited; longjmp(escape,1); }
static int test_dprintf(int fd,const char *fmt,...) { (void)fd;(void)fmt;return 0; }
static void handle_key_progress(char c) { (void)c; }
#define close test_close
#define _exit test_exit
#define dprintf test_dprintf
''' + welcome + back + r'''
static void reset(int authority)
{
    memset(&g_st,0,sizeof(g_st));g_st.screen=SCREEN_DISK;
    strcpy(g_st.admin_pw,"secret");ready=authority;
    queried=entered=destroyed=closed=exited=reexec=0;
}
int main(void)
{
    reset(1);
    handle_back();
    assert(g_st.screen==SCREEN_WELCOME);
    g_st.dirty=0;
    handle_key_welcome('\r');
    assert(g_st.screen==SCREEN_DISK && g_st.dirty && !g_st.admin_pw[0]);
    assert(queried==1 && !entered && !destroyed && !closed && !exited);

    reset(0);handle_back();reexec=1;
    if(setjmp(escape)==0) { handle_key_welcome('\r'); assert(0); }
    assert(queried==1 && entered==1 && destroyed==1 && closed==1 && !exited);

    reset(0);handle_back();
    if(!setjmp(escape)) handle_key_welcome('\r');
    assert(entered==1 && exited==1 && !g_st.admin_pw[0]);

    reset(-1);handle_back();
    if(!setjmp(escape)) handle_key_welcome('\r');
    assert(queried==1 && exited==1 && !entered && !destroyed && !closed);
    puts("GUI navigation: authorized Disk->Back->Next retains window; absent/revoked authority reexecs; query failure stops");
}
'''
    source = tmp / "gui_navigation.c"
    source.write_text(gui_test)
    out = tmp / "gui_navigation"
    run([CC, "-std=c11", "-Wall", "-Wextra",
         "-I" + str(ROOT / "user/lib/libinstall"), str(source), "-o", str(out)])
    run([str(out)])

    # Build the real lightweight archive rule in isolation. No disk/crypto
    # installer objects may be compiled or included by this target.
    run(["make", "-f", str(ROOT / "user/lib/libinstall/Makefile"),
         "libelevate.a", "CC=" + CC,
         "CFLAGS=-O2 -Wall -Wextra -Wno-deprecated-declarations",
         "VPATH=" + str(ROOT / "user/lib/libinstall")], cwd=tmp)
    members = subprocess.check_output(["ar", "-t", str(tmp / "libelevate.a")],
                                     text=True).splitlines()
    assert [m for m in members if m.endswith(".o")] == ["install_elevate.o"], members
    print("archive: real libelevate.a rule contains only install_elevate.o")

    (tmp / "trusted_key.h").write_text(
        "static const unsigned char herald_trusted_key[65] = {0x04};\n")
    for relative in ("user/lib/libauth/auth.c",
                     "user/lib/libinstall/credentials.c",
                     "user/bin/login/main.c", "user/bin/installer/main.c",
                     "user/bin/gui-installer/main.c", "user/bin/useradd/main.c",
                     "user/bin/adminpw/main.c", "user/bin/herald/main.c",
                     "user/bin/lorica-update/main.c"):
        run([CC, "-std=c11", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE",
             "-Wall", "-Wextra", "-Wno-deprecated-declarations",
             "-fsyntax-only", "-I" + str(tmp),
             "-I" + str(ROOT / "user/lib/libauth"),
             "-I" + str(ROOT / "user/lib/libinstall"),
             "-I" + str(ROOT / "user/lib/glyph"), str(ROOT / relative)])
    print("syntax: all nine changed production translation units")
print("auth migration host tests: PASS (mocked Aegis boundary, not an ISO boot)")
