#include "zelr.h"

int main(void);

/* The kernel enters here with a fresh stack and nothing else set up. */
__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    puts("hello from a program the kernel had never seen.\n");
    puts("  pid       "); putn(getpid()); putc('\n');
    puts("  ring      3, reached through int 0x80\n");
    puts("  loaded    from an ELF file on the FAT16 disk\n");

    puts("  arithmetic "); 
    int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    putn(sum);
    puts(" (1..100 summed in user space)\n");
    return 0;
}
