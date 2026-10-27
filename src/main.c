/*
 * main.c - CANopenNode v4 MASTER (Panel Controller) on Zephyr - Network_B CANopen POC
 *
 * Based on CANopenNode/example/main_blank.c
 * (Copyright 2021 Janez Paternoster, Apache-2.0).
 *
 * Threads
 *   main           : stack init, communication-reset loop, CO_process() every ~1 ms,
 *                    prints the Health/Status received from TRM 1 (RPDO1/RPDO2)
 *   tmrTask_thread : SYNC / RPDO / TPDO processing every 1 ms (higher priority)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/reboot.h>     /* sys_reboot() for NMT reset node */
#include <zephyr/logging/log.h>

#include "CANopen.h"
#include "OD.h"
#include "CO_storageBlank.h"
#include "app_uart.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define log_printf(...) LOG_INF(__VA_ARGS__)

/* default values for CO_CANopenInit() */
/* master: goes operational by itself */
#define NMT_CONTROL          (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | \
                              CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME        500
#define SDO_SRV_TIMEOUT_TIME 1000
#define SDO_CLI_TIMEOUT_TIME 500
#define SDO_CLI_BLOCK        false
#define OD_STATUS_BITS       NULL

#define CAN_BITRATE_KBPS     500
#ifdef SYS_CONST_DEBUG
extern struct k_msgq dbg_rx_q;
CO_CANrxMsg_t m;
#endif

/* Global variables and objects */
CO_t* CO = NULL; /* CANopen object */

/* last printed values received from TRM 1 */
static uint8_t  health_last[3];       /* TEC, REC, alive */
static uint32_t status_last[2];       /* alive, state */
static int64_t  lastDiag_ms = 0;        /* last time the CAN controller state was printed */


typedef struct {
    uint8_t  health[3];    /* TEC, REC, alive */
    uint32_t status[2];    /* alive, state    */
} Buffer_t;

typedef struct {
    uint8_t  node;
    Buffer_t now;
    Buffer_t last;
} TRM_t;

static TRM_t trm[] = { { .node = 2 }, { .node = 3 }, { .node = 4 }, { .node = 5 } };

static void trm_print(TRM_t *t)
{
    if (memcmp(t->now.health, t->last.health, sizeof(t->now.health)) != 0) {
        memcpy(t->last.health, t->now.health, sizeof(t->now.health));
        log_printf("RX Health 0x%03X from TRM%u: TEC %u, REC %u, alive %u",
                   0x180U + t->node, t->node,
                   t->now.health[0], t->now.health[1], t->now.health[2]);
    }
    if (memcmp(t->now.status, t->last.status, sizeof(t->now.status)) != 0) {
        memcpy(t->last.status, t->now.status, sizeof(t->now.status));
        log_printf("RX Status 0x%03X from TRM%u: alive %u, state %u",
                   0x280U + t->node, t->node,
                   t->now.status[0], t->now.status[1]);
    }
}

/* main ***********************************************************************/
int
main(void) {
    CO_ReturnError_t err;
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    uint32_t heapMemoryUsed;

    /* CAN controller from the devicetree (chosen zephyr,canbus = FlexCAN on the FRDM-MCXN236) */
    const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
    void* CANptr = (void*)can_dev;                  /* handed to the stack, used by CO_driver.c */

    uint8_t pendingNodeId = CONFIG_MASTER_NODE_ID;  /* master's own node-ID (not a TRM ID) */
    uint8_t activeNodeId = CONFIG_MASTER_NODE_ID;   /* copied from pendingNodeId in the reset section */
    uint16_t pendingBitRate = CAN_BITRATE_KBPS;

    if (!device_is_ready(can_dev)) {
        log_printf("Error: CAN device %s not ready", can_dev->name);
        return 0;
    }
    log_printf("CAN device: %s", can_dev->name);

#if (CO_CONFIG_STORAGE) & CO_CONFIG_STORAGE_ENABLE
    CO_storage_t storage;
    CO_storage_entry_t storageEntries[] = {{.addr = &OD_PERSIST_COMM,
                                            .len = sizeof(OD_PERSIST_COMM),
                                            .subIndexOD = 2,
                                            .attr = CO_storage_cmd | CO_storage_restore,
                                            .addrNV = NULL}};
    uint8_t storageEntriesCount = sizeof(storageEntries) / sizeof(storageEntries[0]);
    uint32_t storageInitError = 0;
#endif

    /* Allocate memory */
    CO_config_t* config_ptr = NULL;
#ifdef CO_MULTIPLE_OD
    /* example usage of CO_MULTIPLE_OD (but still single OD here) */
    CO_config_t co_config = {0};
    OD_INIT_CONFIG(co_config); /* helper macro from OD.h */
    co_config.CNT_LEDS = 1;
    co_config.CNT_LSS_SLV = 1;
    config_ptr = &co_config;
#endif /* CO_MULTIPLE_OD */
    CO = CO_new(config_ptr, &heapMemoryUsed);
    if (CO == NULL) {
        log_printf("Error: Can't allocate memory");
        return 0;
    } else {
        log_printf("Allocated %u bytes for CANopen objects", heapMemoryUsed);
    }

#if (CO_CONFIG_STORAGE) & CO_CONFIG_STORAGE_ENABLE
    err = CO_storageBlank_init(&storage, CO->CANmodule, OD_ENTRY_H1010_storeParameters,
                               OD_ENTRY_H1011_restoreDefaultParameters, storageEntries, storageEntriesCount,
                               &storageInitError);

    if (err != CO_ERROR_NO && err != CO_ERROR_DATA_CORRUPT) {
        log_printf("Error: Storage %u", storageInitError);
        return 0;
    }
#endif

    while (reset != CO_RESET_APP) {
        /* CANopen communication reset - initialize CANopen objects *******************/
        log_printf("CANopenNode - Reset communication...");

        /* Stop the real-time thread from processing PDOs while the stack is rebuilt.
         * Done under the OD lock: the RT thread holds this lock while it processes,
         * so once we have it, the RT thread is guaranteed to be between iterations. */
        CO_LOCK_OD(CO->CANmodule);
        CO->CANmodule->CANnormal = false;
        CO_UNLOCK_OD(CO->CANmodule);

        /* Enter CAN configuration. */
        CO_CANsetConfigurationMode(CANptr);
        CO_CANmodule_disable(CO->CANmodule);

        /* initialize CANopen */
        err = CO_CANinit(CO, CANptr, pendingBitRate);
        if (err != CO_ERROR_NO) {
            log_printf("Error: CAN initialization failed: %d", err);
            return 0;
        }

        CO_LSS_address_t lssAddress = {.identity = {.vendorID = OD_PERSIST_COMM.x1018_identity.vendor_ID,
                                                    .productCode = OD_PERSIST_COMM.x1018_identity.productCode,
                                                    .revisionNumber = OD_PERSIST_COMM.x1018_identity.revisionNumber,
                                                    .serialNumber = OD_PERSIST_COMM.x1018_identity.serialNumber}};
        err = CO_LSSinit(CO, &lssAddress, &pendingNodeId, &pendingBitRate);
        if (err != CO_ERROR_NO) {
            log_printf("Error: LSS slave initialization failed: %d", err);
            return 0;
        }

        activeNodeId = pendingNodeId;
        uint32_t errInfo = 0;

        err = CO_CANopenInit(CO,                   /* CANopen object */
                             NULL,                 /* alternate NMT */
                             NULL,                 /* alternate em */
                             OD,                   /* Object dictionary */
                             OD_STATUS_BITS,       /* Optional OD_statusBits */
                             NMT_CONTROL,          /* CO_NMT_control_t */
                             FIRST_HB_TIME,        /* firstHBTime_ms */
                             SDO_SRV_TIMEOUT_TIME, /* SDOserverTimeoutTime_ms */
                             SDO_CLI_TIMEOUT_TIME, /* SDOclientTimeoutTime_ms */
                             SDO_CLI_BLOCK,        /* SDOclientBlockTransfer */
                             activeNodeId, &errInfo);
        if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
            if (err == CO_ERROR_OD_PARAMETERS) {
                log_printf("Error: Object Dictionary entry 0x%X", errInfo);
            } else {
                log_printf("Error: CANopen initialization failed: %d", err);
            }
            return 0;
        }

        err = CO_CANopenInitPDO(CO, CO->em, OD, activeNodeId, &errInfo);
        if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
            if (err == CO_ERROR_OD_PARAMETERS) {
                log_printf("Error: Object Dictionary entry 0x%X", errInfo);
            } else {
                log_printf("Error: PDO initialization failed: %d", err);
            }
            return 0;
        }

        /* 1 ms timer processing: tmrTask_thread runs continuously (K_THREAD_DEFINE below) */
        /* CAN TX/RX interrupts: handled by the Zephyr driver, see CO_driver.c */

        /* Configure CANopen callbacks, etc */
        if (!CO->nodeIdUnconfigured) {

#if (CO_CONFIG_STORAGE) & CO_CONFIG_STORAGE_ENABLE
            if (storageInitError != 0) {
                CO_errorReport(CO->em, CO_EM_NON_VOLATILE_MEMORY, CO_EMC_HARDWARE, storageInitError);
            }
#endif
        } else {
            log_printf("CANopenNode - Node-id not initialized");
        }

        /* start CAN */
        CO_CANsetNormalMode(CO->CANmodule);

        reset = CO_RESET_NOT;

        log_printf("CANopenNode master - Running, node-ID %u", activeNodeId);

        int64_t lastTicks = k_uptime_ticks();      /* time of the previous loop pass */

        while (reset == CO_RESET_NOT) {
            /* loop for normal program execution ******************************************/
            /* get time difference since last function call */
            int64_t nowTicks = k_uptime_ticks();
            uint32_t timeDifference_us = (uint32_t)k_ticks_to_us_floor64(nowTicks - lastTicks);
            lastTicks = nowTicks;

            /* CANopen process */
            reset = CO_process(CO, false, timeDifference_us, NULL);
            /* DIAG: CAN controller state every 2 s */
            if ((k_uptime_get() - lastDiag_ms) >= 2000) {
                struct can_bus_err_cnt ec = {0};
                enum can_state st;

                lastDiag_ms = k_uptime_get();
                (void)can_get_state(can_dev, &st, &ec);     
                log_printf("CAN state %d (0=active 1=warning 2=passive 3=bus-off), TEC %u, REC %u",
                           st, ec.tx_err_cnt, ec.rx_err_cnt);
            }

            /* Nonblocking application code may go here. */
            /* read OD data: copy under the OD lock, print outside it
             * (member names must match the generated OD.h - search for x2001 / x2002) */
             

            CO_LOCK_OD(CO->CANmodule);
           /* TRM2: RPDO1/2 -> 0x2001 / 0x2002 */
            trm[0].now.health[0] = OD_RAM.x2001_TRM2_Health.TX_Error_Counter;
            trm[0].now.health[1] = OD_RAM.x2001_TRM2_Health.RX_Error_Counter;
            trm[0].now.health[2] = OD_RAM.x2001_TRM2_Health.alive_Counter;
            trm[0].now.status[0] = OD_RAM.x2002_TRM2_Status.alive_Counter;
            trm[0].now.status[1] = OD_RAM.x2002_TRM2_Status.state;

            /* TRM3: 0x2003 / 0x2006 */
            trm[1].now.health[0] = OD_RAM.x2003_TRM3_Health.TX_Error_Counter;
            trm[1].now.health[1] = OD_RAM.x2003_TRM3_Health.RX_Error_Counter;
            trm[1].now.health[2] = OD_RAM.x2003_TRM3_Health.alive_Counter;
            trm[1].now.status[0] = OD_RAM.x2006_TRM3_Status.alive_Counter;
            trm[1].now.status[1] = OD_RAM.x2006_TRM3_Status.state;

            /* TRM4: 0x2004 / 0x2007 */
            trm[2].now.health[0] = OD_RAM.x2004_TRM4_Health.TX_Error_Counter;
            trm[2].now.health[1] = OD_RAM.x2004_TRM4_Health.RX_Error_Counter;
            trm[2].now.health[2] = OD_RAM.x2004_TRM4_Health.alive_Counter;
            trm[2].now.status[0] = OD_RAM.x2007_TRM4_Status.alive_Counter;
            trm[2].now.status[1] = OD_RAM.x2007_TRM4_Status.state;

            /* TRM5: 0x2005 / 0x2008 */
            trm[3].now.health[0] = OD_RAM.x2005_TRM5_Health.TX_Error_Counter;
            trm[3].now.health[1] = OD_RAM.x2005_TRM5_Health.RX_Error_Counter;
            trm[3].now.health[2] = OD_RAM.x2005_TRM5_Health.alive_Counter;
            trm[3].now.status[0] = OD_RAM.x2008_TRM5_Status.alive_Counter;
            trm[3].now.status[1] = OD_RAM.x2008_TRM5_Status.state;
            CO_UNLOCK_OD(CO->CANmodule);
            trm_print(&trm[0]);
            trm_print(&trm[1]);
            trm_print(&trm[2]);
            trm_print(&trm[3]);
            #ifdef SYS_CONST_DEBUG

            while (k_msgq_get(&dbg_rx_q, &m, K_NO_WAIT) == 0) {
            log_printf("bus 0x%03X [%u] %02X %02X %02X %02X %02X %02X %02X %02X",
               m.ident & 0x7FFU, m.DLC,
               m.data[0], m.data[1], m.data[2], m.data[3],
               m.data[4], m.data[5], m.data[6], m.data[7]);
            }
            #endif

            /* print only when a new RPDO changed the values */
            
            /* next step: process BF_Config requests from App_Uart (SDO client) */

            /* Process automatic storage */
            /* (not used: blank storage) */

            /* optional sleep for short time */
            k_msleep(1);
        }
    }

    /* program exit ***************************************************************/
    /* reached only on CO_RESET_APP (NMT "reset node"): restart the whole MCU */
    log_printf("CANopenNode - Reset node");

    /* delete objects from memory */
    CO_CANsetConfigurationMode(CANptr);
    CO_delete(CO);

    /* reset */
    sys_reboot(SYS_REBOOT_COLD);

    return 0;   /* not reached */
}

/* timer thread executes in constant intervals ********************************/
#define RT_THREAD_PERIOD_MS   1
#define RT_THREAD_STACK_SIZE  2048
#define RT_THREAD_PRIORITY    2      /* higher than main (CONFIG_MAIN_THREAD_PRIORITY=5) */

static void
tmrTask_thread(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    int64_t next_ms = k_uptime_get();
    int64_t lastTicks = k_uptime_ticks();

    for (;;) {
        /* sleep until the next 1 ms deadline (absolute: no drift) */
        next_ms += RT_THREAD_PERIOD_MS;
        k_sleep(K_TIMEOUT_ABS_MS(next_ms));

        /* get time difference since last function call */
        int64_t nowTicks = k_uptime_ticks();
        uint32_t timeDifference_us = (uint32_t)k_ticks_to_us_floor64(nowTicks - lastTicks);
        lastTicks = nowTicks;

        if (CO == NULL) {
            continue;   /* main() has not created the stack yet */
        }

        CO_LOCK_OD(CO->CANmodule);
        if (!CO->nodeIdUnconfigured && CO->CANmodule->CANnormal) {
            bool_t syncWas = false;

#if (CO_CONFIG_SYNC) & CO_CONFIG_SYNC_ENABLE
            syncWas = CO_process_SYNC(CO, timeDifference_us, NULL);
#endif
#if (CO_CONFIG_PDO) & CO_CONFIG_RPDO_ENABLE
            CO_process_RPDO(CO, syncWas, timeDifference_us, NULL);
#endif
#if (CO_CONFIG_PDO) & CO_CONFIG_TPDO_ENABLE
            CO_process_TPDO(CO, syncWas, timeDifference_us, NULL);
#endif

            /* Further I/O or nonblocking application code may go here. */
        }
        CO_UNLOCK_OD(CO->CANmodule);

        /* fell behind (e.g. debugger stop): don't burst to catch up */
        if (next_ms < k_uptime_get()) {
            next_ms = k_uptime_get();
        }
    }
}

/* started once at boot by Zephyr */
K_THREAD_DEFINE(tmrTask_id, RT_THREAD_STACK_SIZE, tmrTask_thread, NULL, NULL, NULL,
                RT_THREAD_PRIORITY, 0, 0);