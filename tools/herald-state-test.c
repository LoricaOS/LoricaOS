/* Run production transaction/DB code inside a disposable chroot, never on /. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "transaction.h"

static void dirs(const char *path)
{
    char buf[768];
    assert(strlen(path) < sizeof(buf));
    strcpy(buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
}

static void put(const char *path, const char *bytes)
{
    dirs(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

static void equals(const char *path, const char *bytes)
{
    char buf[512];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, buf, sizeof(buf));
    assert(n == (ssize_t)strlen(bytes));
    assert(memcmp(buf, bytes, (size_t)n) == 0);
    assert(close(fd) == 0);
}

static void stage(const char *path, const char *bytes)
{
    char out[768];
    assert(txn_stage_path(out, sizeof(out), path) == 0);
    put(out, bytes);
}

int main(int argc, char **argv)
{
    if (argc != 2 || chroot(argv[1]) != 0 || chdir("/") != 0) {
        perror("herald state test chroot (run as root)");
        return 1;
    }

    put("/bin/victim", "original");
    put("/var/lib/herald/transaction/state", "applying\n");
    put("/var/lib/herald/transaction/journal", "R\tbin/victim\n");
    put("/var/lib/herald/transaction/backup/bin/victim", "unsigned replacement");
    put("/var/lib/herald/owners/demo.list", "bin/victim\n");
    put("/var/lib/herald/db", "demo\t999\tdemo\tforged\n");
    put("/var/lib/lorica-update/pending", "forged\n");
    assert(txn_recover() == 0);
    equals("/bin/victim", "original");
    equals("/var/lib/herald/transaction/state", "applying\n");
    put("/var/lib/herald/transaction/journal", "N\tbin/victim\n");
    assert(txn_recover() == 0);
    equals("/bin/victim", "original");
    herald_db_entry_t entry;
    assert(db_find("demo", &entry) == 0);

    entry = (herald_db_entry_t){.id = "demo", .version = "1", .exec = "demo"};
    assert(db_put(&entry) == 0);
    assert(txn_begin() == 0);
    assert(txn_remove_owned("demo") == 0);
    assert(txn_commit() == 0);
    equals("/bin/victim", "original");

    /* New-path commit and stale ownership deletion use the same transaction. */
    put("/bin/demo", "old");
    put("/bin/stale", "obsolete");
    put(HERALD_DB_DIR "/owners/demo.list", "bin/demo\nbin/stale\n");
    assert(txn_begin() == 0);
    stage("bin/demo", "new");
    int owner = txn_owner_open("demo");
    assert(owner >= 0 && txn_owner_add(owner, "bin/demo") == 0);
    assert(fsync(owner) == 0 && close(owner) == 0);
    char owner_path[768];
    assert(txn_stage_path(owner_path, sizeof(owner_path),
                         HERALD_DB_DIR "/owners/demo.list" + 1) == 0);
    assert(txn_remove_stale("demo", owner_path) == 0);
    db_set_path(HERALD_TXN_STAGE HERALD_DB_PATH);
    strcpy(entry.version, "2");
    assert(db_put(&entry) == 0);
    db_set_path(NULL);
    assert(txn_commit() == 0);
    equals("/bin/demo", "new");
    assert(access("/bin/stale", F_OK) != 0);
    assert(db_find("demo", &entry) == 1 && !strcmp(entry.version, "2"));
    equals(HERALD_DB_DIR "/owners/demo.list", "bin/demo\n");
    assert(access(HERALD_TXN_DIR, F_OK) != 0);

    /* Interrupted protected recovery restores the backup and leaves legacy bait. */
    put(HERALD_TXN_DIR "/state", "applying\n");
    put(HERALD_TXN_DIR "/journal", "R\tbin/demo\nN\tbin/extra\n");
    put(HERALD_TXN_DIR "/backup/bin/demo", "before interruption");
    put("/bin/extra", "partial");
    assert(txn_recover() == 0);
    equals("/bin/demo", "before interruption");
    assert(access("/bin/extra", F_OK) != 0);
    equals("/var/lib/herald/transaction/state", "applying\n");

    /* Pending kernel rollback and completion retain their protected metadata. */
    assert(txn_begin() == 0);
    stage("bin/demo", "pending replacement");
    stage(HERALD_UPDATE_PENDING + 1, "pending\n");
    assert(txn_commit() == 0 && txn_recover() == 1);
    assert(txn_rollback() == 0);
    equals("/bin/demo", "before interruption");
    assert(access(HERALD_UPDATE_PENDING, F_OK) != 0);
    assert(txn_begin() == 0);
    stage("bin/demo", "finished replacement");
    stage(HERALD_UPDATE_PENDING + 1, "pending\n");
    assert(txn_commit() == 0 && txn_recover() == 1);
    assert(txn_finalize() == 0 && txn_recover() == 0);
    equals("/bin/demo", "finished replacement");
    assert(access(HERALD_UPDATE_PENDING, F_OK) != 0);
    assert(access(HERALD_TXN_DIR, F_OK) != 0);
    equals("/bin/victim", "original");
    /* A file transferred from package A to B is deleted then replaced in one
     * transaction. Rollback must restore the original, not replay R then N
     * and unlink it. Exercise both explicit rollback and crash recovery. */
    for (int recovery = 0; recovery < 2; recovery++) {
        put("/bin/transferred", "original owner A");
        put(HERALD_DB_DIR "/owners/owner-a.list", "bin/transferred\n");
        assert(txn_begin() == 0);
        assert(txn_remove_owned("owner-a") == 0);
        stage("bin/transferred", "replacement owner B");
        stage(HERALD_UPDATE_PENDING + 1, "pending\n");
        assert(txn_commit() == 0 && txn_recover() == 1);
        equals("/bin/transferred", "replacement owner B");
        if (recovery) {
            put(HERALD_TXN_DIR "/state", "applying\n");
            assert(txn_recover() == 0);
        } else {
            assert(txn_rollback() == 0);
        }
        equals("/bin/transferred", "original owner A");
        equals(HERALD_DB_DIR "/owners/owner-a.list", "bin/transferred\n");
        assert(access(HERALD_UPDATE_PENDING, F_OK) != 0);
        assert(txn_recover() == 0);
        equals("/bin/transferred", "original owner A");
    }
    puts("herald state: forged legacy inputs ignored; protected commit/recovery/rollback/owners PASS");
    return 0;
}
