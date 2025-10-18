#include "kernel/types.h"
#include "user/user.h"
#include "kernel/riscv.h"
#include "kernel/param.h"

int
main(int argc, char *argv[])
{
    int total = 200; // allocate more than MAXRESHEAP
    char *base = sbrk(PGSIZE * total);
    if (base == (char*)-1) {
        printf("sbrk failed\n");
        exit(1);
    }

    // Touch pages 0..49 (hot set), 50..199 cold
    for (int iter = 0; iter < 1000; iter++) {
        // touch hot set
        for (int i = 0; i < 50; i++) {
            base[i*PGSIZE] = (char)i;
        }
        // occasionally touch some cold pages
        if (iter % 50 == 0) {
            for (int i = 50; i < total; i += 30) {
                base[i*PGSIZE] = 1;
            }
        }
    }

    printf("WSA test done\n");
    exit(0);
}
