/*
 * CAN module object for generic microcontroller.
 *
 * This file is a template for other microcontrollers.
 *
 * @file        CO_driver.c
 * @ingroup     CO_driver
 * @author      Janez Paternoster
 * @copyright   2004 - 2020 Janez Paternoster
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

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

#include "301/CO_driver.h"

LOG_MODULE_REGISTER(co_driver, LOG_LEVEL_INF);
#define SYS_CONST_DEBUG 1
/* Mutexes behind the CO_LOCK_* macros in CO_driver_target.h */
K_MUTEX_DEFINE(CO_send_mutex);
K_MUTEX_DEFINE(CO_emcy_mutex);
K_MUTEX_DEFINE(CO_od_mutex);

#if SYS_CONST_DEBUG
K_MSGQ_DEFINE(dbg_rx_q, sizeof(CO_CANrxMsg_t), 64, 4);   /* at file scope */
#endif
static void CO_CANrx_isr(const struct device* dev, struct can_frame* frame, void* user_data);
static void CO_CANtx_isr(const struct device* dev, int error, void* user_data);
static void CO_CANtx_retry(struct k_work* work);
static int  send_to_hw(CO_CANmodule_t* CANmodule, const CO_CANtx_t* buffer);

void
CO_CANsetConfigurationMode(void* CANptr) {
    /* Put CAN module in configuration mode */
    const struct device *dev = CANptr;
    if(dev != NULL)
    {
        (void)can_stop(dev); /* Stop CAN device */
        /* Set CAN module to configuration mode */
    }

}

void
CO_CANsetNormalMode(CO_CANmodule_t* CANmodule) {
    /* Put CAN module in normal mode */
    const struct device *dev = CANmodule->CANptr;
    int err = can_start(dev);

    /* Put CAN module in normal mode */
    if ((err == 0) || (err == -EALREADY)) {
        CANmodule->CANnormal = true;
    } else {
        LOG_ERR("CO_driver: can_start failed (%d)\n", err);
    }
}

CO_ReturnError_t
CO_CANmodule_init(CO_CANmodule_t* CANmodule, void* CANptr, CO_CANrx_t rxArray[], uint16_t rxSize, CO_CANtx_t txArray[],
                  uint16_t txSize, uint16_t CANbitRate) {
    uint16_t i;
    int err;
    const struct device *dev = CANptr;

    /* verify arguments */
    if (CANmodule == NULL || dev == NULL || rxArray == NULL || txArray == NULL) {
        LOG_ERR("CO_driver: CO_CANmodule_init failed (NULL argument)\n");
        return CO_ERROR_ILLEGAL_ARGUMENT;
        
    }
    //zephyr is ready
     if (!device_is_ready(dev)) {
        LOG_ERR("CO_driver: CAN device not ready\n");
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    if (CANmodule->txWorkInitialised) {
        struct k_work_sync sync;

        /* remove a pending retry and wait for a running one to finish,
         * so k_work_init() below is safe */
        k_work_cancel_sync(&CANmodule->txRetryWork, &sync);
    }
    if (CANmodule->rxFilterAdded) {
        can_remove_rx_filter(dev, CANmodule->rxFilterId);
        CANmodule->rxFilterAdded = false;
    }

    

    /* Configure object variables */
    CANmodule->CANptr = CANptr;
    CANmodule->rxArray = rxArray;
    CANmodule->rxSize = rxSize;
    CANmodule->txArray = txArray;
    CANmodule->txSize = txSize;
    CANmodule->CANerrorStatus = 0;
    CANmodule->CANnormal = false;
    CANmodule->useCANrxFilters = false;
    CANmodule->bufferInhibitFlag = false;
    CANmodule->firstCANtxMessage = true;
    CANmodule->CANtxCount = 0U;
    CANmodule->errOld = 0U;

    for (i = 0U; i < rxSize; i++) {
        rxArray[i].ident = 0U;
        rxArray[i].mask = 0xFFFFU;
        rxArray[i].object = NULL;
        rxArray[i].CANrx_callback = NULL;
    }
    for (i = 0U; i < txSize; i++) {
        txArray[i].bufferFull = false;
    }
       k_work_init(&CANmodule->txRetryWork, CO_CANtx_retry);
        CANmodule->txWorkInitialised = true;

    /* Configure CAN module registers */
    err = can_set_mode(dev, CAN_MODE_NORMAL);   /* CAN_MODE_LOOPBACK for single-board tests */
    if (err != 0) {
        LOG_ERR("CO_driver: can_set_mode failed (%d)\n", err);
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }


    /* Configure CAN timing */
    err = can_set_bitrate(dev, (uint32_t)CANbitRate*1000U);  //kbps - bps
    if(err != 0) {
        LOG_ERR("CO_driver: can_set_bitrate failed (%d)\n", err);
        return CO_ERROR_ILLEGAL_BAUDRATE;
    }

    /* Configure CAN module hardware filters */
    if (CANmodule->useCANrxFilters) {
        /* CAN module filters are used, they will be configured with */
        /* CO_CANrxBufferInit() functions, called by separate CANopen */
        /* init functions. */
        /* Configure all masks so, that received CAN frame must match filter */
    } else {
        struct can_filter filter = {
            .id = 0,
            .mask = 0,
            .flags=0
        };
        /* CAN module filters are not used, all CAN frames with standard 11-bit */
        /* identifier will be received */
        /* Configure mask 0 so, that all CAN frames with standard identifier are accepted */
        /* configure CAN interrupt registers */
    err = can_add_rx_filter(dev, CO_CANrx_isr, CANmodule, &filter);

        if (err < 0) {
            LOG_ERR("CO_driver: can_add_rx_filter failed (%d)\n", err);
            return CO_ERROR_ILLEGAL_ARGUMENT;
        }
        CANmodule->rxFilterId = err;
        CANmodule->rxFilterAdded = true;
    }

    

    return CO_ERROR_NO;
}

void
CO_CANmodule_disable(CO_CANmodule_t* CANmodule) {
    int err;
     

    if ((CANmodule == NULL) || (CANmodule->CANptr == NULL)) {
        return;
    }

    err = can_stop(CANmodule->CANptr);
    if ((err != 0) && (err != -EALREADY)) {
        LOG_ERR("can_stop failed (%d)", err);
    }

    if (CANmodule->rxFilterAdded) {
        can_remove_rx_filter(CANmodule->CANptr, CANmodule->rxFilterId);
        CANmodule->rxFilterAdded = false;
    }

    CANmodule->CANnormal = false;
}

CO_ReturnError_t
CO_CANrxBufferInit(CO_CANmodule_t* CANmodule, uint16_t index, uint16_t ident, uint16_t mask, bool_t rtr, void* object,
                   void (*CANrx_callback)(void* object, void* message)) {
   CO_ReturnError_t ret = CO_ERROR_NO;

    if ((CANmodule != NULL) && (object != NULL) && (CANrx_callback != NULL) && (index < CANmodule->rxSize)) {
        /* buffer, which will be configured */
        CO_CANrx_t* buffer = &CANmodule->rxArray[index];

        /* Turn the entry OFF while it is changed: the RX interrupt skips
         * entries with a NULL callback (needed when the stack reconfigures
         * an RX buffer while CAN is running, e.g. RPDO COB-ID changed via SDO). */
        buffer->CANrx_callback = NULL;
        CO_MemoryBarrier();

        /* Configure object variables */
        buffer->object = object;

        /* CAN identifier and mask; bit 11 (0x0800) represents RTR */
        buffer->ident = ident & 0x07FFU;
        if (rtr) {
            buffer->ident |= 0x0800U;
        }
        buffer->mask = (mask & 0x07FFU) | 0x0800U;   /* RTR bit must always match */

        /* Set CAN hardware module filter and mask. */
        if (CANmodule->useCANrxFilters) {
            /* Not used in the POC: one catch-all filter is added in CO_CANmodule_init()
             * and frames are matched in software in the RX callback. */
        }

        /* Turn the entry ON last, once all fields are consistent */
        CO_MemoryBarrier();
        buffer->CANrx_callback = CANrx_callback;
    } else {
        LOG_ERR("CO_CANrxBufferInit: illegal argument (index %u)", index);
        ret = CO_ERROR_ILLEGAL_ARGUMENT;
    }

    return ret;
}

CO_CANtx_t*
CO_CANtxBufferInit(CO_CANmodule_t* CANmodule, uint16_t index, uint16_t ident, bool_t rtr, uint8_t noOfBytes,
                   bool_t syncFlag) {
    CO_CANtx_t* buffer = NULL;

    if ((CANmodule != NULL) && (index < CANmodule->txSize)) {
        /* get specific buffer */
        buffer = &CANmodule->txArray[index];

        /* Alligned with Zephyr structure */
       buffer->ident = (uint32_t)ident & 0x07FFU;
        if (rtr) {
            buffer->ident |= 0x0800U;
        }
        buffer->DLC = (noOfBytes > 8U) ? 8U : noOfBytes;   /* classic CAN: max 8 bytes */
        buffer->bufferFull = false;
        buffer->syncFlag = syncFlag;
    }

    return buffer;
}

/* Build a Zephyr frame from a CANopenNode TX buffer and hand it to the driver.
 * Returns the Zephyr result: 0 = accepted, -EAGAIN = no free mailbox, <0 = error. */
static int send_to_hw(CO_CANmodule_t *CANmodule, const CO_CANtx_t *buffer)
{
	struct can_frame frame = {0};

	frame.id    = buffer->ident & 0x07FFU;
	frame.flags = ((buffer->ident & 0x0800U) != 0U) ? CAN_FRAME_RTR : 0U;
	frame.dlc   = buffer->DLC;
	memcpy(frame.data, buffer->data, buffer->DLC);

	return can_send(CANmodule->CANptr, &frame, K_NO_WAIT, CO_CANtx_isr, CANmodule);
}

CO_ReturnError_t
CO_CANsend(CO_CANmodule_t* CANmodule, CO_CANtx_t* buffer) {
    CO_ReturnError_t err = CO_ERROR_NO;   /* CANopen result returned to the stack */
    int ret;                              /* Zephyr result of can_send() */

    /* Verify overflow: previous frame in this buffer not sent yet */
    if (buffer->bufferFull) {
        if (!CANmodule->firstCANtxMessage) {
            /* don't set error, if bootup message is still on buffers */
            CANmodule->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
        }
        LOG_WRN("TX overflow, ID 0x%03X", (unsigned)(buffer->ident & 0x07FFU));
        err = CO_ERROR_TX_OVERFLOW;
    }

    CO_LOCK_CAN_SEND(CANmodule);

    /* Send now only if nothing is queued (keeps order) */
    if (CANmodule->CANtxCount == 0U) {
        ret = send_to_hw(CANmodule, buffer);
    } else {
        ret = -EAGAIN;                    /* older frames waiting: queue behind them */
    }

    if (ret == 0) {
        /* accepted by a hardware mailbox */
        CANmodule->bufferInhibitFlag = buffer->syncFlag;
    } else {
        if (ret != -EAGAIN) {
            /* e.g. -ENETUNREACH (bus-off): keep it queued, it is sent once the
             * bus recovers; CO_CANmodule_process() reports the bus state */
            LOG_WRN("can_send failed (%d), frame queued", ret);
        }
        /* queue it; the TX-done callback triggers sending the queue */
        if (!buffer->bufferFull) {
            buffer->bufferFull = true;
            CANmodule->CANtxCount++;
        }
    }

    CO_UNLOCK_CAN_SEND(CANmodule);

    return err;
}
void
CO_CANclearPendingSyncPDOs(CO_CANmodule_t* CANmodule) {
    uint32_t tpdoDeleted = 0U;

    CO_LOCK_CAN_SEND(CANmodule);
    /* Abort frame from CAN module, if there is synchronous TPDO.
     * Take special care with this functionality. */
    if (/* CAN frameIsOnCanBuffer && */ CANmodule->bufferInhibitFlag) {
        /* clear TXREQ */
        CANmodule->bufferInhibitFlag = false;
        tpdoDeleted = 1U;
    }
    /* delete also pending synchronous TPDOs in TX buffers */
    if (CANmodule->CANtxCount != 0U) {
        uint16_t i;
        CO_CANtx_t* buffer = &CANmodule->txArray[0];
        for (i = CANmodule->txSize; i > 0U; i--) {
            if (buffer->bufferFull) {
                if (buffer->syncFlag) {
                    buffer->bufferFull = false;
                    CANmodule->CANtxCount--;
                    tpdoDeleted = 2U;
                }
            }
            buffer++;
        }
    }
    CO_UNLOCK_CAN_SEND(CANmodule);

    if (tpdoDeleted != 0U) {
        CANmodule->CANerrorStatus |= CO_CAN_ERRTX_PDO_LATE;
    }
}

/* Get error counters from the module. If necessary, function may use different way to determine errors. */
//static uint16_t rxErrors = 0, txErrors = 0, overflow = 0;


void CO_CANmodule_process(CO_CANmodule_t* CANmodule) {
    uint32_t err;
    enum can_state state;
    struct can_bus_err_cnt err_cnt;
    uint32_t overflow = 0;
    int ret;
    
    ret = can_get_state(CANmodule->CANptr, &state, &err_cnt);
    if (ret != 0) {
        LOG_ERR("Unable to get CAN state %d", ret);
        return;                         /* state / err_cnt not valid */
    }
    err = ((uint32_t)err_cnt.tx_err_cnt << 16) | ((uint32_t)err_cnt.rx_err_cnt << 8) | overflow;
    if (state == CAN_STATE_BUS_OFF) {
        err |= 0x80000000U;             /* TEC is 8-bit and cannot show bus-off */
    }
    if (CANmodule->errOld != err) {
        uint16_t status = CANmodule->CANerrorStatus;

        CANmodule->errOld = err;

        if (state == CAN_STATE_BUS_OFF) {
            /* bus off */
            status |= CO_CAN_ERRTX_BUS_OFF;
        } else {
            /* recalculate CANerrorStatus, first clear some flags */
            status &= 0xFFFF
                      ^ (CO_CAN_ERRTX_BUS_OFF | CO_CAN_ERRRX_WARNING | CO_CAN_ERRRX_PASSIVE | CO_CAN_ERRTX_WARNING
                         | CO_CAN_ERRTX_PASSIVE);

            /* rx bus warning or passive */
            if (err_cnt.rx_err_cnt >= 128) {
                status |= CO_CAN_ERRRX_WARNING | CO_CAN_ERRRX_PASSIVE;
            } else if (err_cnt.rx_err_cnt >= 96) {
                status |= CO_CAN_ERRRX_WARNING;
            }

            /* tx bus warning or passive */
            if (err_cnt.tx_err_cnt >= 128) {
                status |= CO_CAN_ERRTX_WARNING | CO_CAN_ERRTX_PASSIVE;
            } else if (err_cnt.tx_err_cnt >= 96) {
                status |= CO_CAN_ERRTX_WARNING;
            }

            /* if not tx passive clear also overflow */
            if ((status & CO_CAN_ERRTX_PASSIVE) == 0) {
                status &= 0xFFFF ^ CO_CAN_ERRTX_OVERFLOW;
            }
        }

        if (overflow != 0) {
            /* CAN RX bus overflow */
            status |= CO_CAN_ERRRX_OVERFLOW;
        }

        CANmodule->CANerrorStatus = status;
    }

    /* Safety net: restart the TX queue if frames are waiting but no TX-done
     * interrupt will come to trigger it (e.g. frames queued during bus-off). */
    if ((CANmodule->CANtxCount > 0U) && (state != CAN_STATE_BUS_OFF)) {
        k_work_submit(&CANmodule->txRetryWork);
    }
}



/* Replaces the "transmit interrupt" branch of the template's CO_CANinterrupt().
 * Called by the Zephyr CAN driver (given to can_send()) when a frame was sent
 * or failed. Runs in interrupt context: it may not take CO_LOCK_CAN_SEND (a mutex),
 * so sending the queue is handed to a work item that runs in a thread. */
static void
CO_CANtx_isr(const struct device *dev, int error, void *user_data) {
    CO_CANmodule_t* CANmodule = user_data;   /* passed to can_send() */

    ARG_UNUSED(dev);

    if (error == 0) {
        /* First CAN frame (bootup) was sent successfully */
        CANmodule->firstCANtxMessage = false;
    }
    /* clear flag from previous CAN frame */
    CANmodule->bufferInhibitFlag = false;

    /* Are there any new CAN frames waiting to be sent? */
    if (CANmodule->CANtxCount > 0U) {
        k_work_submit(&CANmodule->txRetryWork);
    }
}

/* Work item (thread context): send queued frames in array order until the
 * hardware mailboxes are full again. */
static void
CO_CANtx_retry(struct k_work* work) {
    CO_CANmodule_t* CANmodule = CONTAINER_OF(work, CO_CANmodule_t, txRetryWork);
    uint16_t i;
    uint16_t pending = 0U;
    bool blocked = false;

    CO_LOCK_CAN_SEND(CANmodule);

    for (i = 0U; i < CANmodule->txSize; i++) {
        CO_CANtx_t* buffer = &CANmodule->txArray[i];

        if (!buffer->bufferFull) {
            continue;
        }
        if (!blocked && (send_to_hw(CANmodule, buffer) == 0)) {
            /* accepted by a mailbox */
            buffer->bufferFull = false;
            CANmodule->bufferInhibitFlag = buffer->syncFlag;
        } else {
            /* no mailbox free: keep this and all later ones queued, in order */
            blocked = true;
            pending++;
        }
    }
    CANmodule->CANtxCount = pending;

    CO_UNLOCK_CAN_SEND(CANmodule);
}
/* Replaces the "receive interrupt" branch of the template's CO_CANinterrupt().
 * Called by the Zephyr CAN driver (registered in CO_CANmodule_init() with
 * can_add_rx_filter) for every received frame. Runs in interrupt context:
 * no blocking calls, no mutexes, keep it short. */
static void
CO_CANrx_isr(const struct device* dev, struct can_frame* frame, void* user_data) {
    CO_CANmodule_t* CANmodule = user_data;   /* passed to can_add_rx_filter() */
    CO_CANrxMsg_t rcvMsg;
    uint16_t index;
    CO_CANrx_t* buffer;

    ARG_UNUSED(dev);

    /* CANopen uses standard 11-bit identifiers only */
    if ((frame->flags & CAN_FRAME_IDE) != 0U) {
        return;
    }

    /* Convert the Zephyr frame; same convention as CO_CANrxBufferInit(): bit 11 = RTR */
    rcvMsg.ident = (uint16_t)(frame->id & 0x07FFU);
    if ((frame->flags & CAN_FRAME_RTR) != 0U) {
        rcvMsg.ident |= 0x0800U;
    }
    rcvMsg.DLC = (frame->dlc > 8U) ? 8U : frame->dlc;
    memcpy(rcvMsg.data, frame->data, rcvMsg.DLC);
    #if SYS_CONST_DEBUG
    if (rcvMsg.ident >= 0x0E0U && rcvMsg.ident <= 0x0FFU) {
    (void)k_msgq_put(&dbg_rx_q, &rcvMsg, K_NO_WAIT);   /* drop if full, never block */
    }
    #endif

    /* Catch-all filter: search rxArray for the same CAN-ID; skip unused entries */
    buffer = &CANmodule->rxArray[0];
    for (index = CANmodule->rxSize; index > 0U; index--) {
        void (*callback)(void* object, void* message) = buffer->CANrx_callback;

        if ((callback != NULL) && (((rcvMsg.ident ^ buffer->ident) & buffer->mask) == 0U)) {
            callback(buffer->object, (void*)&rcvMsg);
            break;
        }
        buffer++;
    }
}

