/*
 * app_uart.c - MASTER: UART command input + BF_Config download via SDO
 *
 * Type a TRM node-ID (e.g. "2") and press Enter: the master writes 8 bytes of
 * BF_Config to that TRM's OD entry 0x2000:00 using the SDO client, waits for
 * the TRM's confirmation and prints OK or the SDO abort code.
 *
 * Only this thread uses the SDO client (CO->SDOclient). CO_process() in the
 * main thread does not touch it, so no further locking is needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include "CANopen.h"
#include "app_uart.h"

/* the CANopen object, created in main.c */
extern CO_t* CO;

/* BF_Config on every TRM: OD entry 0x2000, sub-index 0, 8 bytes */
#define BF_CONFIG_INDEX      0x2000U
#define BF_CONFIG_SUBINDEX   0x00U
#define BF_CONFIG_SIZE       16
#define BF_SDO_TIMEOUT_MS    1000U
//LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

//#define log_printf(...) LOG_INF(__VA_ARGS__)

#define APP_UART_STACK_SIZE  2048
#define APP_UART_PRIORITY    7      /* lowest: waits for typing almost all the time */
uint64_t BFConfig_Counter =0;
/* POC: dummy BF_Config per TRM, byte 0 = target node-ID so the TRM print shows who it was for */
static void
get_bf_config(uint8_t nodeId, uint8_t cfg[BF_CONFIG_SIZE]) {
    static const uint8_t pattern[BF_CONFIG_SIZE] = {0x00, 0x22, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD, 0x10,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11};

    memcpy(cfg, pattern, BF_CONFIG_SIZE);
    cfg[0] = nodeId;
    cfg[7]= BFConfig_Counter++;
}

uint32_t
bf_config_send(uint8_t nodeId, const uint8_t* cfg, size_t len) {
    CO_SDOclient_t* sdo;
    CO_SDO_abortCode_t abortCode = CO_SDO_AB_NONE;
    CO_SDO_return_t ret;

    if ((CO == NULL) || (CO->SDOclient == NULL)) {
        printk("SDO client not available (stack not running or CO_CONFIG_SDO_CLI disabled)\n");
        return (uint32_t)CO_SDO_AB_GENERAL;
    }
    sdo = CO->SDOclient;

    /* point the client at this TRM: request on 0x0C0+n, response on 0x0E0+n */
    ret = CO_SDOclient_setup(sdo, 0x0C0U + nodeId, 0x0E0U + nodeId, nodeId);
    if (ret != CO_SDO_RT_ok_communicationEnd) {
        printk("SDO setup for node %u failed (%d)\n", nodeId, ret);
        return (uint32_t)CO_SDO_AB_GENERAL;
    }

    /* start "write index:subindex, len bytes" (no block transfer for 8 bytes) */
    ret = CO_SDOclientDownloadInitiate(sdo, BF_CONFIG_INDEX, BF_CONFIG_SUBINDEX, len, BF_SDO_TIMEOUT_MS,
                                       false);
    if (ret != CO_SDO_RT_ok_communicationEnd) {
        printk("SDO download initiate for node %u failed (%d)\n", nodeId, ret);
        return (uint32_t)CO_SDO_AB_GENERAL;
    }

    /* hand over the data */
    (void)CO_SDOclientDownloadBufWrite(sdo, cfg, len);

    /* run the transfer until it ends: > 0 = in progress, 0 = done, < 0 = error */
    int64_t last = k_uptime_ticks();
    do {
        k_msleep(1);
        int64_t now = k_uptime_ticks();
        uint32_t diff_us = (uint32_t)k_ticks_to_us_floor64(now - last);
        last = now;

        ret = CO_SDOclientDownload(sdo, diff_us, false, false, &abortCode, NULL, NULL);
    } while (ret > 0);

    return (ret == CO_SDO_RT_ok_communicationEnd) ? 0U : (uint32_t)abortCode;
}

static void
app_uart_thread(void* ptr1, void* ptr2, void* ptr3) {
    ARG_UNUSED(ptr1);
    ARG_UNUSED(ptr2);
    ARG_UNUSED(ptr3);

    console_getline_init(); /* once, before the loop */
    printk("Type a TRM node-ID (1..127) and Enter to send BF_Config, e.g. 2\n");

    while (1) {
        char* line = console_getline(); /* blocks until Enter */

        if ((line == NULL) || (line[0] == '\0')) {
            continue; /* empty line: ignore */
        }

        char* end;
        long node = strtol(line, &end, 10); /* text -> number */

        if ((*end != '\0') || (node < 1) || (node > 127)) {
            printk("Invalid input '%s'. Type a node-ID 1..127, e.g. 2\n", line);
            continue;
        }

        uint8_t nodeId = (uint8_t)node;
        uint8_t cfg[BF_CONFIG_SIZE];

        get_bf_config(nodeId, cfg);
        printk("TX BF_Config to TRM%u via SDO 0x%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", nodeId,
               0x0C0U + nodeId, cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6], cfg[7], cfg[8], cfg[9], cfg[10], cfg[11], cfg[12], cfg[13], cfg[14], cfg[15]);

        uint32_t abortCode = bf_config_send(nodeId, cfg, sizeof(cfg));

        if (abortCode == 0U) {
            printk("BF_Config to TRM%u: OK (confirmed by the TRM)\n", nodeId);
        } else {
            printk("BF_Config to TRM%u: FAILED, SDO abort code 0x%08X\n", nodeId, abortCode);
        }
    }
}

/* defined after the function, so the name is known here; started at boot */
K_THREAD_DEFINE(app_uart_tid, APP_UART_STACK_SIZE, app_uart_thread, NULL, NULL, NULL, APP_UART_PRIORITY, 0, 0);
