/* SPDX-License-Identifier: GPL-3.0-only */


#include "hs_aml_uart.h"
#include <asm/termbits.h>
#include <asm/ioctls.h>
#include <asm/fcntl.h>

#define FIELD(native, local) \
    _Static_assert(offsetof(struct termios2, native) == \
                   offsetof(struct hs_aml_uart_termios2, local), "termios2 " #native)

_Static_assert(sizeof(struct termios2) == sizeof(struct hs_aml_uart_termios2), "termios2 size");
FIELD(c_iflag, iflag);
FIELD(c_oflag, oflag);
FIELD(c_cflag, cflag);
FIELD(c_lflag, lflag);
FIELD(c_line, line);
FIELD(c_cc, cc);
FIELD(c_ispeed, ispeed);
FIELD(c_ospeed, ospeed);
_Static_assert(NCCS == 19 && VTIME == 5 && VMIN == 6, "control characters");
_Static_assert(TCGETS2 == HS_AML_UART_TCGETS2, "TCGETS2");
_Static_assert(TCSETS2 == HS_AML_UART_TCSETS2, "TCSETS2");
_Static_assert(TIOCEXCL == HS_AML_UART_TIOCEXCL, "TIOCEXCL");
_Static_assert(TIOCNXCL == HS_AML_UART_TIOCNXCL, "TIOCNXCL");
_Static_assert((O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW) ==
               HS_AML_UART_OPEN_FLAGS, "ARM Linux open flags");
_Static_assert(CBAUD == 0x100f && CIBAUD == 0x100f0000 && BOTHER == 0x1000 &&
               IBSHIFT == 16, "baud mask ABI");
_Static_assert((IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON) ==
               0x5eb, "recovered input clear mask");
_Static_assert((ISIG | ICANON | ECHO | ECHONL | IEXTEN) == 0x804b,
               "recovered local clear mask");
_Static_assert((CSIZE | CREAD | PARENB | CLOCAL) == 0x9b0 &&
               (CS8 | CREAD | CLOCAL) == 0x8b0, "recovered control masks");
