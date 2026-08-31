#ifndef HERALD_TRANSACTION_H
#define HERALD_TRANSACTION_H

#include <stddef.h>

#define HERALD_TXN_DIR   "/var/lib/herald/transaction"
#define HERALD_TXN_STAGE HERALD_TXN_DIR "/stage"

/* One crash-recoverable root-filesystem transaction. Paths are relative to /. */
int txn_recover(void);
int txn_finalize(void);
int txn_rollback(void);
int txn_begin(void);
int txn_commit(void);
void txn_abort(void);
int txn_stage_existing(const char *path);
int txn_stage_path(char *out, size_t size, const char *path);
int txn_owner_open(const char *id);
int txn_owner_add(int fd, const char *path);
int txn_remove_stale(const char *id, const char *new_owner_path);
int txn_remove_owned(const char *id);

#endif
