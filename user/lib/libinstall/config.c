/* config.c — installed-system boot config + test binary strip (libinstall) */
#include "libinstall.h"
#include <unistd.h>

/* install_write_boot_cfg — installed-system boot configuration step.
 *
 * With Limine this is a no-op. The installer raw-copies the prebuilt esp.img
 * (Limine's BOOTX64.EFI + an installed-mode limine.conf — see
 * tools/gen-limine-conf.sh) to the target disk's ESP partition. That
 * limine.conf loads the kernel directly from the ext2 Aegis-root partition by
 * its GPT type GUID, so nothing boot-related needs to be written onto the ext2
 * filesystem. (Under GRUB this wrote /boot/grub/grub.cfg on the ext2 root.)
 * Retained as a step for install-progress UX and ABI stability. */
int install_write_boot_cfg(void)
{
    return 0;
}

void install_strip_test_binaries(void)
{
    unlink("/bin/thread_test");
    unlink("/bin/mmap_test");
    unlink("/bin/proc_test");
    unlink("/bin/pty_test");
    unlink("/bin/dynlink_test");
}
