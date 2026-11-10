/* Fixed administrative executable boundary, shared by login and libelevate.
 * Never add an interpreter, shell, caller-selected path or PATH lookup here. */
#ifndef ADMIN_TARGETS_H
#define ADMIN_TARGETS_H

#include <stdint.h>
#include <string.h>

#define ADMIN_CAP_AUTH       (1u << 4)
#define ADMIN_CAP_SETUID     (1u << 6)
#define ADMIN_CAP_DISK       (1u << 11)
#define ADMIN_CAP_INSTALL    (1u << 17)
#define ADMIN_READY_MARKER   "--admin-ready"
#define ADMIN_GUI_INSTALLER_TARGET "/apps/gui-installer/gui-installer"

static inline uint32_t admin_target_caps(const char *path)
{
    if (!path) return 0;
    if (!strcmp(path, "/bin/installer") || !strcmp(path, ADMIN_GUI_INSTALLER_TARGET))
        return ADMIN_CAP_AUTH | ADMIN_CAP_SETUID | ADMIN_CAP_DISK | ADMIN_CAP_INSTALL;
    if (!strcmp(path, "/bin/useradd"))
        return ADMIN_CAP_AUTH | ADMIN_CAP_SETUID | ADMIN_CAP_INSTALL;
    if (!strcmp(path, "/bin/adminpw"))
        return ADMIN_CAP_AUTH | ADMIN_CAP_INSTALL;
    if (!strcmp(path, "/bin/herald"))
        return ADMIN_CAP_INSTALL;
    if (!strcmp(path, "/bin/lorica-update") || !strcmp(path, "/bin/lorica-recover"))
        return ADMIN_CAP_DISK;
    return 0;
}

#endif
