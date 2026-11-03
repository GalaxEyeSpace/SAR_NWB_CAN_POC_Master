/*
 * Device and application specific definitions for CANopenNode.
 *
 * @file        CO_driver_target.h
 * @author      Janez Paternoster
 * @copyright   2021 Janez Paternoster
 *
 * This file is part of <https://github.com/CANopenNode/CANopenNode>, a CANopen Stack.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

#ifndef CO_DRIVER_TARGET_H
#define CO_DRIVER_TARGET_H

/* This file contains device and application specific definitions. It is included from CO_driver.h, which contains
 * documentation for common definitions below. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>        /* struct k_work, struct k_mutex */
#include <zephyr/drivers/can.h>
#include <zephyr/sys/barrier.h>   /* barrier_dmem_fence_full() */

#ifdef CO_DRIVER_CUSTOM
#include "CO_driver_custom.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Stack configuration override default values. For more information see file CO_config.h. */

/* MASTER only: SDO client (segmented transfer for > 4 bytes) and the FIFO it needs */
#define CO_CONFIG_SDO_CLI (CO_CONFIG_SDO_CLI_ENABLE | CO_CONFIG_SDO_CLI_SEGMENTED | \
                           CO_CONFIG_SDO_CLI_LOCAL | CO_CONFIG_GLOBAL_FLAG_CALLBACK_PRE | \
                           CO_CONFIG_GLOBAL_FLAG_TIMERNEXT)
#define CO_CONFIG_FIFO    (CO_CONFIG_FIFO_ENABLE)
#define CO_CONFIG_NMT  (CO_CONFIG_NMT_MASTER | CO_CONFIG_GLOBAL_FLAG_CALLBACK_PRE | \
                        CO_CONFIG_GLOBAL_FLAG_TIMERNEXT)
/* Basic definitions. If big endian, CO_SWAP_xx macros must swap bytes. */
#define CO_LITTLE_ENDIAN
#define CO_SWAP_16(x) x
#define CO_SWAP_32(x) x
#define CO_SWAP_64(x) x
/* NULL is defined in stddef.h */
/* true and false are defined in stdbool.h */
/* int8_t to uint64_t are defined in stdint.h */
typedef uint_fast8_t bool_t;
typedef float float32_t;
typedef double float64_t;

/* Received CAN frame, as built by CO_CANrx_isr() in CO_driver.c */
typedef struct {
    uint16_t ident;   /* 11-bit ID, bit 11 = RTR */
    uint8_t  DLC;
    uint8_t  data[8];
} CO_CANrxMsg_t;

/* Access to received CAN frame: the stack reads every received frame ONLY through
 * these macros. They must read the real fields of CO_CANrxMsg_t above.*/
 
#define CO_CANrxMsg_readIdent(msg) ((uint16_t)(((const CO_CANrxMsg_t*)(msg))->ident & 0x07FFU))
#define CO_CANrxMsg_readDLC(msg)   ((uint8_t)(((const CO_CANrxMsg_t*)(msg))->DLC))
#define CO_CANrxMsg_readData(msg)  ((const uint8_t*)(((const CO_CANrxMsg_t*)(msg))->data))

/* Received frame object */
typedef struct {
    uint16_t ident;
    uint16_t mask;
    void* object;
    void (*CANrx_callback)(void* object, void* message);
} CO_CANrx_t;

/* Transmit frame object */
typedef struct {
    uint32_t ident;
    uint8_t DLC;
    uint8_t data[8];
    volatile bool_t bufferFull;
    volatile bool_t syncFlag;
} CO_CANtx_t;

/* CAN module object */
typedef struct {
    void* CANptr;
    CO_CANrx_t* rxArray;
    uint16_t rxSize;
    CO_CANtx_t* txArray;
    uint16_t txSize;
    uint16_t CANerrorStatus;
    volatile bool_t CANnormal;
    volatile bool_t useCANrxFilters;
    volatile bool_t bufferInhibitFlag;
    volatile bool_t firstCANtxMessage;
    volatile uint16_t CANtxCount;
    uint32_t errOld;

     /* Zephyr specific */
    int rxFilterId;              /* id returned by can_add_rx_filter() */
    bool rxFilterAdded;          /* true while that filter is registered */
    struct k_work txRetryWork;   /* sends the TX queue when a mailbox frees */
    bool txWorkInitialised;      /* true once k_work_init() was called */
} CO_CANmodule_t;

/* Data storage object for one entry */
typedef struct {
    void* addr;
    size_t len;
    uint8_t subIndexOD;
    uint8_t attr;
    /* Additional variables (target specific) */
    void* addrNV;
} CO_storage_entry_t;

/* Critical sections. Used only from threads (main loop, 1 ms thread, UART thread,
 * TX retry work item), never from the CAN interrupt, so Zephyr mutexes are safe.
 * Zephyr mutexes are recursive for the owning thread and have priority inheritance.
 * The mutexes are defined once in CO_driver.c (K_MUTEX_DEFINE).
 * (The template versions were empty: several threads changed the TX queue and
 * the OD at the same time without protection.) */
extern struct k_mutex CO_send_mutex;
extern struct k_mutex CO_emcy_mutex;
extern struct k_mutex CO_od_mutex;

/* (un)lock critical section in CO_CANsend() */
#define CO_LOCK_CAN_SEND(CAN_MODULE)    k_mutex_lock(&CO_send_mutex, K_FOREVER)
#define CO_UNLOCK_CAN_SEND(CAN_MODULE)  k_mutex_unlock(&CO_send_mutex)

/* (un)lock critical section in CO_errorReport() or CO_errorReset() */
#define CO_LOCK_EMCY(CAN_MODULE)        k_mutex_lock(&CO_emcy_mutex, K_FOREVER)
#define CO_UNLOCK_EMCY(CAN_MODULE)      k_mutex_unlock(&CO_emcy_mutex)

/* (un)lock critical section when accessing Object Dictionary */
#define CO_LOCK_OD(CAN_MODULE)          k_mutex_lock(&CO_od_mutex, K_FOREVER)
#define CO_UNLOCK_OD(CAN_MODULE)        k_mutex_unlock(&CO_od_mutex)

/* Synchronization between CAN receive (interrupt) and data processing threads. */
#define CO_MemoryBarrier()  barrier_dmem_fence_full()
#define CO_FLAG_READ(rxNew) ((rxNew) != NULL)
#define CO_FLAG_SET(rxNew)                                                                                             \
    do {                                                                                                               \
        CO_MemoryBarrier();                                                                                            \
        rxNew = (void*)1L;                                                                                             \
    } while (0)
#define CO_FLAG_CLEAR(rxNew)                                                                                           \
    do {                                                                                                               \
        CO_MemoryBarrier();                                                                                            \
        rxNew = NULL;                                                                                                  \
    } while (0)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CO_DRIVER_TARGET_H */
