/* COM1. This is the kernel's lifeline: QEMU can pipe it straight to a
   terminal, so the automated tests read the OS's output from here. */
#include "serial.h"
#include "io.h"
#include "signal.h"
#include "idt.h"
#include "pic.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* interrupts off while configuring */
    outb(COM1 + 3, 0x80);   /* enable divisor latch */
    outb(COM1 + 0, 0x03);   /* 38400 baud, low byte */
    outb(COM1 + 1, 0x00);   /* high byte */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0x07);   /* enable + clear FIFO, interrupt on every byte */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR set */
}

/* Polling the UART loses bytes: the FIFO is only 16 deep and a host that
   pipes a script in sends far faster than a task can poll. Take the
   interrupt and buffer instead. */
#define RXSZ 512
static volatile char rx[RXSZ];
static volatile u32 rx_head = 0, rx_tail = 0;

static volatile u32 rx_overruns = 0;
static volatile u32 rx_isr_bytes = 0, rx_read_bytes = 0, rx_isr_calls = 0;
u32 serial_overruns(void) { return rx_overruns; }
u32 serial_isr_bytes(void) { return rx_isr_bytes; }
u32 serial_read_bytes(void) { return rx_read_bytes; }
u32 serial_isr_calls(void) { return rx_isr_calls; }

static void serial_isr(registers_t *r) {
    (void)r;
    rx_isr_calls++;
    /* Service until the UART says it has nothing pending. Reading IIR is
       what acknowledges the interrupt on a 16550; only draining RBR can
       leave the line asserted and the next byte unreported. */
    for (int guard = 0; guard < 64; guard++) {
        u8 iir = inb(COM1 + 2);
        if (iir & 0x01) break;              /* no interrupt pending */
        u8 lsr = inb(COM1 + 5);
        if (lsr & 0x02) rx_overruns++;      /* the UART itself lost a byte */
        while (inb(COM1 + 5) & 1) {
            char c = (char)inb(COM1);
            rx_isr_bytes++;

            /* An interrupt has to be noticed here rather than where this
               ring is drained, because while the program being interrupted
               is running nobody drains it: the shell that would is blocked
               waiting for that very program. The byte is kept as well as
               acted on, so a shell at its prompt still sees it and can
               throw away the line it had half typed. */
            if (c == 3) signal_interrupt();

            u32 next = (rx_head + 1) % RXSZ;
            if (next != rx_tail) { rx[rx_head] = c; rx_head = next; }
            else rx_overruns++;             /* our ring was full */
        }
    }
}

void serial_enable_irq(void) {
    register_interrupt_handler(36, serial_isr);   /* IRQ4 */
    outb(COM1 + 1, 0x01);                         /* data-available interrupt */
    pic_unmask(4);
}

bool serial_buffered(void) { return rx_head != rx_tail; }

int serial_trygetc(void) {
    if (rx_head == rx_tail) return -1;
    char c = rx[rx_tail];
    rx_tail = (rx_tail + 1) % RXSZ;
    rx_read_bytes++;
    return (u8)c;
}

static int tx_ready(void) { return inb(COM1 + 5) & 0x20; }

void serial_putc(char c) {
    while (!tx_ready()) { }
    outb(COM1, (u8)c);
}

void serial_write(const char *s) { while (*s) serial_putc(*s++); }

bool serial_has_input(void) { return (inb(COM1 + 5) & 1) != 0; }
char serial_getc(void) { while (!serial_has_input()) { } return (char)inb(COM1); }
