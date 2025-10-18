#include "kernel/types.h"
#include "user/user.h"
#include "kernel/riscv.h"
#include "kernel/param.h"

int
main(int argc, char *argv[])
{
    int npages = 200; 
    void* heappages = sbrk(4096*npages);
    if (!heappages) {
        printf("[X] Heap memory allocation FAILED.\n");
        return -1;
    }

    int* a;
    // Hot Set: 0-100 , Cold Set: 100-199
    for (int iter = 0; iter < 1000; iter++) {
        // Hot Pages
        for (int i = 0; i < 100; i++) {
            a = (int*)(&heappages[i*PGSIZE]);
            for (int j = 0; j < PGSIZE/sizeof(int); j++) {
                *a = j;
                a++;
            }
        }
        // uint64 t = read_current_timestamp();
        // Cold Pages
        if (iter % 50 == 0) {
            for (int i = 100; i < npages; i += 30) {
                a = (int*)(&heappages[i*PGSIZE]);
                *a = 1;
                // printf("%d\n", i);
            }
        }
    }

    printf("WSA test done\n");
    exit(0);
}
