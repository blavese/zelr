/* The two 8259 PICs power up mapped over the CPU's exception vectors, so the
   first job is to move them to 32..47. */
#include "pic.h"
#include "io.h"

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

void pic_init(void) {
    u8 m1 = inb(PIC1_DAT), m2 = inb(PIC2_DAT);

    outb(PIC1_CMD, 0x11); io_wait();   /* start init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, 0x20); io_wait();   /* master vector offset -> 32 */
    outb(PIC2_DAT, 0x28); io_wait();   /* slave  vector offset -> 40 */
    outb(PIC1_DAT, 0x04); io_wait();   /* slave is on master IRQ2 */
    outb(PIC2_DAT, 0x02); io_wait();
    outb(PIC1_DAT, 0x01); io_wait();   /* 8086 mode */
    outb(PIC2_DAT, 0x01); io_wait();

    outb(PIC1_DAT, m1);
    outb(PIC2_DAT, m2);
}

void pic_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_disable(void) {
    outb(PIC1_DAT, 0xFF);
    outb(PIC2_DAT, 0xFF);
}

void pic_mask(u8 irq) {
    u16 port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    if (irq >= 8) irq -= 8;
    outb(port, (u8)(inb(port) | (1 << irq)));
}

void pic_unmask(u8 irq) {
    u16 port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    if (irq >= 8) irq -= 8;
    outb(port, (u8)(inb(port) & ~(1 << irq)));
}
