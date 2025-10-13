#include "../kernel/types.h"
#include "user.h"

int
main(void)
{
    printf("Testing sandbox...\n");
    interpose(1 << SYS_open, "-");

    int fd = open("README", 0);
    if(fd < 0)
        printf("open() blocked by sandbox as expected!\n");
    else
        printf("open() succeeded unexpectedly!\n");

    exit(0);
}