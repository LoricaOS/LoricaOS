#include <unistd.h>
int main(void) {
    write(1, "\033[2J\033[H", 7);
    return 0;
}
