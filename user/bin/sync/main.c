/* sync — flush filesystem buffers. */
#include <unistd.h>

int
main(void)
{
    sync();
    return 0;
}
