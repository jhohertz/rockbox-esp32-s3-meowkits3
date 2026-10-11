/***************************************************************************
 * Bring-up trace on UART0.
 *
 * See bootlog.c. Only useful while the target is being brought up; every call
 * is one character at 115200 baud through the mask ROM.
 ****************************************************************************/

#ifndef BOOTLOG_H
#define BOOTLOG_H

void boot_trace(char c);
void boot_trace_hex(unsigned int v);
void boot_trace_raw(unsigned int v);

#endif /* BOOTLOG_H */
