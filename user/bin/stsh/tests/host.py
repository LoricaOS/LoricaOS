#!/usr/bin/env python3
"""Focused shell migration regressions; stdlib-only, no kernel/custom syscalls."""
import os
from pathlib import Path
import pty
import resource
import select
import shlex
import signal
import subprocess
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]


class ShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="stsh-host-")
        cls.root = Path(cls.build.name)
        cls.shell = cls.root / "stsh"
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c11", "-D_GNU_SOURCE", "-g", "-O1", "-Wall", "-Wextra",
                 "-Wno-missing-field-initializers", "-Wno-misleading-indentation"]
        objects = []
        for name in ["main", "run", "lexer", "exec", "env"]:
            obj = cls.root / (name + ".o")
            subprocess.run(cc + flags + [
                f'-DSTSH_PATH="{cls.shell}"', "-Dfork=test_fork", "-Dpipe=test_pipe",
                "-Dopen=test_open", "-Dwrite=test_write", "-Dread=test_read",
                "-Ddup2=test_dup2", "-Dsetpgid=test_setpgid",
                "-Dfopen=test_fopen", "-c", str(SOURCE / (name + ".c")), "-o", str(obj)],
                check=True)
            objects.append(str(obj))
        subprocess.run(cc + flags + objects + [str(SOURCE / "tests/host_shim.c"),
                                               "-o", str(cls.shell)], check=True)
        cls.fallback = cls.root / "shell"
        fallback_obj = cls.root / "fallback.o"
        subprocess.run(cc + flags + ["-Dfork=test_fork", "-Dpipe=test_pipe",
                                     "-Dopen=test_open", "-Dwrite=test_write",
                                     "-Dread=test_read", "-Ddup2=test_dup2",
                                     "-Dsetpgid=test_setpgid",
                                     "-c", str(SOURCE.parent / "shell/main.c"),
                                     "-o", str(fallback_obj)], check=True)
        subprocess.run(cc + flags + [str(fallback_obj), str(SOURCE / "tests/host_shim.c"),
                                     "-o", str(cls.fallback)], check=True)
        # The real capability UX must at least compile, but is never invoked
        # using platform-dependent Aegis syscall numbers on the host.
        subprocess.run(cc + flags + ["-fsyntax-only", str(SOURCE / "caps.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="stsh-case-")
        self.cwd = Path(self.work.name)
        self.addCleanup(self.work.cleanup)

    def run_sh(self, text, status=0, env=None, fallback=False, nofile=None,
               closed=(), input_text=None):
        e = {"PATH": "/bin:/usr/bin"}
        if env:
            e.update(env)
        def setup():
            if nofile:
                resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))
            for fd in closed:
                os.close(fd)
        with subprocess.Popen([str(self.fallback if fallback else self.shell), "-c", text],
                              cwd=self.cwd, env=e,
                              stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              preexec_fn=setup if nofile or closed else None,
                              start_new_session=True) as proc:
            try:
                out, err = proc.communicate(input=input_text, timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                raise
            r = subprocess.CompletedProcess(proc.args, proc.returncode, out, err)
        self.assertEqual(r.returncode, status, (text, r.stdout, r.stderr))
        self.assertNotIn("returned 124", r.stderr, "old fork image used authority")
        return r

    def read(self, path):
        return (self.cwd / path).read_text()

    def test_parent_opens_and_append(self):
        (self.cwd / "in").write_text("hello\n")
        self.run_sh("cat <in >out; cat <in >>out")
        self.assertEqual(self.read("out"), "hello\nhello\n")

    def test_ordered_dup_and_repeated_outputs(self):
        r = self.run_sh("/bin/sh -c 'echo out; echo err >&2' 2>&1 >out")
        self.assertEqual(r.stdout, "err\n")
        self.assertEqual(self.read("out"), "out\n")
        self.run_sh("/bin/sh -c 'echo out; echo err >&2' >both 2>&1")
        self.assertEqual(self.read("both"), "out\nerr\n")
        self.run_sh("echo x >first >second")
        self.assertEqual(self.read("first"), "")
        self.assertEqual(self.read("second"), "x\n")
        r = self.run_sh("echo x >&2")
        self.assertEqual(r.stderr, "x\n")

    def test_failure_order_and_diagnostic_destination(self):
        self.run_sh("echo x <missing >not-created", status=1)
        self.assertFalse((self.cwd / "not-created").exists())
        self.run_sh("echo x >created <missing >not-created", status=1)
        self.assertEqual(self.read("created"), "")
        self.assertFalse((self.cwd / "not-created").exists())
        r = self.run_sh("echo x 2>errors <missing", status=1)
        self.assertIn("missing", self.read("errors"))
        self.assertEqual(r.stderr, "")
        self.run_sh("echo x <missing >$(echo side >side; echo late)", status=1)
        self.assertFalse((self.cwd / "side").exists())

    def test_builtin_restore_after_failure(self):
        r = self.run_sh("true >created <missing; echo restored")
        self.assertEqual(r.stdout, "restored\n")
        self.assertEqual(self.read("created"), "")
        r = self.run_sh("true >a >b; echo restored")
        self.assertEqual(r.stdout, "restored\n")
        self.assertEqual(self.read("a"), "")
        self.assertEqual(self.read("b"), "")

    def test_builtin_restores_closed_standard_descriptors(self):
        self.run_sh("true >first; /bin/cat >second", closed=(1,), input_text="payload\n")
        self.assertEqual(self.read("first"), "")
        self.assertEqual(self.read("second"), "payload\n")
        # Restoration closes descriptors that were initially closed, rather
        # than retaining a redirect or accidentally using another saved fd.
        (self.cwd / "in").write_text("input\n")
        self.run_sh("true <in; /bin/cat >second", closed=(0,), status=1)
        self.assertEqual(self.read("second"), "")
        self.run_sh("true <in >first 2>errors; /bin/cat <in >second",
                    closed=(0, 1, 2))
        self.assertEqual(self.read("second"), "input\n")

    def test_command_bypasses_function_during_prepare_and_bootstrap(self):
        r = self.run_sh("""echo() { /bin/echo FUNCTION; }
command echo EXTERNAL
command echo PIPELINE | cat
x=$(command echo CAPTURE)
/bin/echo "$x"
""")
        self.assertEqual(r.stdout, "EXTERNAL\nPIPELINE\nCAPTURE\n")
        r = self.run_sh("""echo() { command echo WRAPPER "$1"; }
echo direct
echo pipe | cat
x=$(echo capture)
/bin/echo "$x"
echo background >wrapped &
""")
        self.assertEqual(r.stdout, "WRAPPER direct\nWRAPPER pipe\nWRAPPER capture\n")
        self.assertEqual(self.read("wrapped"), "WRAPPER background\n")

    def test_interactive_state_jobs_survive_ctrl_c_and_restore_terminal(self):
        pid, master = pty.fork()
        if pid == 0:
            os.chdir(self.cwd)
            os.execve(str(self.shell), [str(self.shell)], {"PATH": "/bin:/usr/bin"})
        transcript = bytearray()

        def prompt():
            start = len(transcript)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if b"[HOST] prompt\r\n" in transcript[start:]:
                    return
                if select.select([master], [], [], 0.1)[0]:
                    try:
                        data = os.read(master, 4096)
                    except OSError as error:
                        self.fail(f"interactive shell died: {error}: {bytes(transcript)!r}")
                    self.assertTrue(data, bytes(transcript))
                    transcript.extend(data)
            self.fail(f"no prompt after Ctrl-C: {bytes(transcript)!r}")

        try:
            prompt()
            self.assertEqual(os.tcgetpgrp(master), pid)
            for text in ["( /bin/sleep 5 )", "x=$(/bin/sleep 5)",
                         "( ( /bin/sleep 5 ) )",
                         "f() { x=$(/bin/sleep 5); }; f </dev/null"]:
                os.write(master, (text + "\n").encode())
                deadline = time.monotonic() + 2
                while os.tcgetpgrp(master) == pid and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertNotEqual(os.tcgetpgrp(master), pid,
                                    f"state job never gained its own group: {text}")
                os.write(master, b"\x03")
                prompt()
                self.assertEqual(os.tcgetpgrp(master), pid, bytes(transcript))
                os.kill(pid, 0)
                start = len(transcript)
                os.write(master, b"/bin/echo JOB-STATUS:$?\n")
                prompt()
                self.assertIn(b"JOB-STATUS:130\r\n", transcript[start:])
            os.write(master, b"/bin/echo SHELL-SURVIVED\n")
            prompt()
            self.assertIn(b"SHELL-SURVIVED\r\n", transcript)
        finally:
            # An assertion must not leave a stopped shell or foreground job.
            try:
                foreground = os.tcgetpgrp(master)
                if foreground > 1 and foreground != pid:
                    os.killpg(foreground, signal.SIGKILL)
            except OSError:
                pass
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.close(master)
            os.waitpid(pid, 0)

    def test_literal_heredoc_and_short_writes(self):
        r = self.run_sh("cat <<'END'\n$x 'quoted'\nEND\n",
                        env={"STSH_TEST_SHORT_WRITE": "1", "STSH_TEST_WRITE_EINTR": "1"})
        self.assertEqual(r.stdout, "$x 'quoted'\n")
        r = self.run_sh("cat <<END\n" + "x" * 3999 + "\nEND\n")
        self.assertEqual(r.stdout, "x" * 3999 + "\n")
        r = self.run_sh("cat <<END\n" + "x" * 4000 + "\nEND\n", status=1)
        self.assertIn("heredoc", r.stderr)
        r = self.run_sh("cat <<END\nbody\nEND\n", status=1,
                        env={"STSH_TEST_WRITE_ZERO": "1"})
        self.assertIn("heredoc", r.stderr)

    def test_heredoc_then_input_and_input_then_heredoc(self):
        (self.cwd / "in").write_text("file\n")
        r = self.run_sh("cat <in <<END\nbody\nEND\n")
        self.assertEqual(r.stdout, "body\n")
        r = self.run_sh("cat <<END <in\nbody\nEND\n")
        self.assertEqual(r.stdout, "file\n")
        self.run_sh("cat <missing <<END\nbody\nEND\n", status=1)
        self.run_sh("cat <<END <missing >never\nbody\nEND\n", status=1)
        self.assertFalse((self.cwd / "never").exists())
        r = self.run_sh("cat <<A <<B\nfirst\nA\nsecond\nB\n")
        self.assertEqual(r.stdout, "second\n")
        r = self.run_sh("cat <<-END\n\tbody\n\tEND\n")
        self.assertEqual(r.stdout, "body\n")

    def test_heredoc_pipeline_background_and_capture_roundtrip(self):
        self.run_sh("cat <<END | cat >pipe\nliteral $x\nEND\n")
        self.assertEqual(self.read("pipe"), "literal $x\n")
        self.run_sh("cat <<END >bg &\nbackground\nEND\n")
        self.assertEqual(self.read("bg"), "background\n")
        r = self.run_sh("""f() { cat <<END | cat
raw ' " $x
END
}
x=$(f)
echo "$x"
""")
        self.assertEqual(r.stdout, "raw ' \" $x\n")

    def test_pipeline_and_background(self):
        self.run_sh("echo hello | cat >out")
        self.assertEqual(self.read("out"), "hello\n")
        self.run_sh("echo hello >stage | cat >out")
        self.assertEqual(self.read("stage"), "hello\n")
        self.assertEqual(self.read("out"), "")
        self.run_sh("echo hello | cat <missing >never", status=1)
        self.assertFalse((self.cwd / "never").exists())
        self.run_sh("cat <missing >never &", status=1)
        self.assertFalse((self.cwd / "never").exists())
        self.run_sh("echo bg | cat >bg &")
        self.assertEqual(self.read("bg"), "bg\n")

    def test_fork_and_pipe_failure_cleanup(self):
        # Earlier stage would otherwise run forever; a failed downstream fork
        # must close pipes, kill and reap it, not wait indefinitely.
        self.run_sh("sleep 60 | cat", status=1, env={"STSH_TEST_FORK_FAIL": "2"})
        self.run_sh("sleep 60 | cat | cat", status=1,
                    env={"STSH_TEST_PIPE_FAIL": "2"})
        self.run_sh("echo x | cat >out; echo recovered",
                    env={"STSH_TEST_FORK_FAIL": "2"})
        self.run_sh("/bin/sleep 60 | cat", status=1, fallback=True,
                    env={"STSH_TEST_FORK_FAIL": "2"})

    def test_repeated_pipeline_preparation_does_not_leak_fds(self):
        self.run_sh('i=0; while test "$i" -lt 100; do echo x | cat >out; i=$((i+1)); done',
                    nofile=16)
        self.assertEqual(self.read("out"), "x\n")

    def test_capture_locals_functions_params_status(self):
        r = self.run_sh("""local=private
set -- 'one two' "three'four"
f() { echo "$local:$1"; return 7; }
x=$(f "$2")
echo "$?:$x:$1:$2"
( local=changed; echo "$local:$1"; exit 9 )
echo "$?:$local"
""")
        self.assertEqual(r.stdout,
                         "7:private:three'four:one two:three'four\nchanged:one two\n9:private\n")

    def test_capture_prior_status_arg0_and_literal_tokens(self):
        r = self.run_sh("""set -- 'a b'
f() { cat <<'END'
$literal ' " ; |
END
return 6; }
false
x=$(echo "$?:$0:$1"; f)
echo "$?:$x"
""")
        self.assertEqual(r.stdout, f"6:1:{self.shell}:a b\n$literal ' \" ; |\n")

    def test_no_capless_builtin_in_capture_subshell_pipeline_background(self):
        r = self.run_sh("x=$(caps); echo \"$x\"; ( caps ); caps | cat; caps >bg &")
        self.assertEqual(r.stdout, "fresh-caps\nfresh-caps\nfresh-caps\n")
        self.assertEqual(self.read("bg"), "fresh-caps\n")

    def test_function_pipeline_and_background_argument_quoting(self):
        r = self.run_sh("""v=private
f() { echo "$v:$1"; }
f "a'b;$v" | cat
f 'bg' >bg &
echo "$v"
""")
        self.assertEqual(r.stdout, "private:a'b;private\nprivate\n")
        self.assertEqual(self.read("bg"), "private:bg\n")

    def test_subshell_function_definitions_do_not_escape(self):
        r = self.run_sh("f() { echo original; }; ( f() { echo child; }; f ); f")
        self.assertEqual(r.stdout, "child\noriginal\n")

    def test_subshell_skip_never_expands_or_executes(self):
        r = self.run_sh("""( case "$(echo once >>count; echo x)" in x) echo yes;; esac )
false
( echo "$?" )
""")
        self.assertEqual(r.stdout, "yes\n1\n")
        self.assertEqual(self.read("count"), "once\n")

    def test_empty_params_nested_capture_and_function_status(self):
        r = self.run_sh("""set -- '' 'a b'
f() { echo "$#:$1:$2"; return 4; }
x=$(f "$1" "$2")
echo "$?:$x"
( true )
echo "$?"
""")
        self.assertEqual(r.stdout, "4:2::a b\n0\n")
        r = self.run_sh("( ( false ) )", status=1)
        self.assertEqual(r.stdout, "")

    def test_child_failure_is_reported_by_parent(self):
        r = self.run_sh("echo x >out", status=125, env={"STSH_TEST_DUP2_FAIL": "1"})
        self.assertIn("child setup failed", r.stderr)
        self.assertEqual(self.read("out"), "")
        r = self.run_sh("not-a-command", status=127)
        self.assertIn("exec failed", r.stderr)
        r = self.run_sh("not-a-command & sleep 0.1")
        self.assertIn("background job", r.stderr)

    def test_bounded_state_creation_rejects_oversize(self):
        for i in range(6):
            (self.cwd / f"func{i}").write_text(f"f{i}() {{ : '" + "x" * 12000 + "'; }\n")
        r = self.run_sh("; ".join(f". func{i}" for i in range(6)) + "; x=$(true)",
                        status=2)
        self.assertIn("interpreter state exceeds", r.stderr)

    def test_bootstrap_rejects_malformed_and_oversize(self):
        # state header, status, zero env, arg0, zero params/functions/tokens.
        valid = "5:STSH10:0:1:x0:0:0:"
        for payload in ["", valid[:-1], valid + "junk", "5:STSH20:0:1:x0:0:0:",
                        "5:STSH199999999999:", "x" * 65536,
                        "5:STSH10:0:1:x65:", "5:STSH10:0:1:x0:65:",
                        "5:STSH10:0:1:x0:0:1:999:0:",
                        "5:STSH10:0:1:x0:0:1:1:1:x",
                        "5:STSH10:0:1:x0:2:1:f0:1:f0:0:"]:
            r = subprocess.run([str(self.shell), "--state-v1", payload],
                               text=True, capture_output=True, timeout=8)
            self.assertEqual(r.returncode, 2, payload[:100])
            self.assertIn("interpreter state", r.stderr)
        r = subprocess.run([str(self.shell), "--state-v1", valid],
                           text=True, capture_output=True, timeout=8)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_minimal_shell_parent_opens_order_and_status(self):
        (self.cwd / "in").write_text("fallback\n")
        self.run_sh("cat <in >out", fallback=True)
        self.assertEqual(self.read("out"), "fallback\n")
        self.run_sh("cat <in >>out", fallback=True)
        self.assertEqual(self.read("out"), "fallback\nfallback\n")
        self.run_sh("cat <missing >never", status=1, fallback=True)
        self.assertFalse((self.cwd / "never").exists())
        self.run_sh("echo x >a >b", fallback=True)
        self.assertEqual(self.read("a"), "")
        self.assertEqual(self.read("b"), "x\n")
        self.run_sh("/usr/bin/false" if Path("/usr/bin/false").exists() else "/bin/false",
                    status=1, fallback=True)
        self.run_sh("echo x | cat >pipe", fallback=True)
        self.assertEqual(self.read("pipe"), "x\n")
        emit = self.cwd / "emit"
        emit.write_text("#!/bin/sh\necho out\necho err >&2\n")
        emit.chmod(0o755)
        r = self.run_sh("./emit 2>&1 >out", fallback=True)
        self.assertEqual(r.stdout, "err\n")
        self.assertEqual(self.read("out"), "out\n")
        r = self.run_sh("exec /no-such-command", status=127, fallback=True)
        self.assertIn("exec", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
