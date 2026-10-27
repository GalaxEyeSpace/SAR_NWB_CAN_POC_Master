/*
 * app_uart.h - MASTER: UART command input + BF_Config download via SDO
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_UART_H_
#define APP_UART_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Write BF_Config to one TRM (OD 0x2000:00) via SDO and wait for the result.
 * Must only be called from one thread (the SDO client is not shared).
 * Returns 0 on success, otherwise the SDO abort code.
 */
uint32_t bf_config_send(uint8_t nodeId, const uint8_t* cfg, size_t len);



#endif /* APP_UART_H_ */
