/* SPDX-License-Identifier: GPL-3.0-only */




#include "hs_aml_uart_linux.h"
int main(void)
{
    struct hs_aml_uart_linux adapter = HS_AML_UART_LINUX_INIT;
    const struct hs_aml_uart_linux_identity invalid = {0};
    return hs_aml_uart_linux_init(&adapter, 0U, &invalid, NULL, NULL) ? 1 : 0;
}
