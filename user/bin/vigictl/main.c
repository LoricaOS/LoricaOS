#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VIGIL_CMD_PATH "/run/vigil.cmd"
#define VIGIL_PID_PATH "/run/vigil.pid"

static void
usage(void)
{
    write(2,
        "usage: vigictl <command> [service]\n"
        "  status              - list all services\n"
        "  start <svc>         - start a service\n"
        "  stop <svc>          - stop a service\n"
        "  restart <svc>       - restart a service\n"
        "  shutdown            - graceful shutdown\n",
        169);
}

static pid_t
read_vigil_pid(void)
{
    char buf[32];
    int fd = open(VIGIL_PID_PATH, O_RDONLY);
    if (fd < 0) {
        write(2, "vigil: not running (no pid file at /run/vigil.pid)\n", 51);
        return -1;
    }
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (pid_t)atoi(buf);
}

static int
write_cmd(const char *cmd)
{
    int fd = open(VIGIL_CMD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        write(2, "vigictl: cannot write cmd file\n", 31);
        return -1;
    }
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    close(fd);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    pid_t pid = read_vigil_pid();
    if (pid <= 0) {
        return 1;
    }

    if (strcmp(cmd, "shutdown") == 0) {
        kill(pid, SIGTERM);
        return 0;
    }

    char buf[192];
    if (strcmp(cmd, "status") == 0) {
        snprintf(buf, sizeof(buf), "status");
    } else if ((strcmp(cmd, "start")   == 0 ||
                strcmp(cmd, "stop")    == 0 ||
                strcmp(cmd, "restart") == 0) && argc >= 3) {
        snprintf(buf, sizeof(buf), "%s %s", cmd, argv[2]);
    } else {
        usage();
        return 1;
    }

    if (write_cmd(buf) < 0) return 1;
    kill(pid, SIGUSR1);
    return 0;
}
