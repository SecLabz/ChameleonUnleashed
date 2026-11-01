#include "dataframe.h"
#include "netdata.h"
#include "usb_main.h"
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#define NRF_LOG_MODULE_NAME data_frame
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

typedef struct {
    netdata_frame_raw_t buffer;
    uint16_t position, command, status, length;
    atomic_bool complete, processing;
} receiver_t;
static receiver_t receivers[DATA_SOURCE_COUNT];
static data_source_t processing_source;
static atomic_uint generations[DATA_SOURCE_COUNT];
static uint32_t processing_generation;
static netdata_frame_raw_t m_netdata_frame_tx_buf;
static data_frame_tx_t m_frame_tx_buf_info = { .buffer = (uint8_t *)&m_netdata_frame_tx_buf };
static data_frame_cbk_t m_frame_process_cbk;

data_source_t data_frame_source(void) { return processing_source; }

bool data_frame_reply_valid(void) {
    return generations[processing_source] == processing_generation;
}

bool data_frame_usb_pending(uint16_t command) {
    receiver_t *rx = &receivers[DATA_SOURCE_USB];
    return rx->complete && rx->command == command;
}

bool data_frame_usb_inside_frame(void) {
    receiver_t *rx = &receivers[DATA_SOURCE_USB];
    return rx->position > 0 && !rx->complete && !rx->processing;
}

void data_frame_invalidate(data_source_t source) {
    if (source >= DATA_SOURCE_COUNT) return;
    generations[source]++;
}

void data_frame_disconnect(data_source_t source) {
    if (source >= DATA_SOURCE_COUNT) return;
    data_frame_invalidate(source);
    receiver_t *rx = &receivers[source];
    if (rx->processing) return;
    rx->complete = false;
    rx->position = 0;
}

static uint8_t compute_lrc(uint8_t *buf, uint16_t bufsize) {
    uint8_t lrc = 0x00;
    for (uint16_t i = 0; i < bufsize; i++) {
        lrc += buf[i];
    }
    return 0x100 - lrc;
}

//
//  !!!!!!!!!!!!!!!!! NRF_LOG_HEXDUMP_INFO() printing long data can cause freezing and needs to be fixed. !!!!!!!!!!!!!!!!!
//  FIXME.
//

/**
 * @brief: create a packet, put the created data packet into the buffer, and wait for the post to set up a non busy state
 * @param cmd: instructionResponse
 * @param status:responseStatus
 * @param length: answerDataLength
 * @param data: answerData
 */
data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status, uint16_t data_length, uint8_t *data) {
    if (data_length > 0 && data == NULL) {
        NRF_LOG_ERROR("data_frame_make error, null pointer.");
        return NULL;
    }
    if (data_length > 4096) {
        NRF_LOG_ERROR("data_frame_make error, too much data.");
        return NULL;
    }

    // NRF_LOG_INFO("TX Data frame: cmd = 0x%04x (%i), status = 0x%04x, length = %d%s", cmd, cmd, status, data_length, data_length > 0 ? ", data =" : "");
    // if (data_length > 0) {
    //     NRF_LOG_HEXDUMP_INFO(data, data_length);
    // }

    netdata_frame_postamble_t *tx_post = (netdata_frame_postamble_t *)((uint8_t *)&m_netdata_frame_tx_buf + sizeof(netdata_frame_preamble_t) + data_length);
    // sof
    m_netdata_frame_tx_buf.pre.sof = NETDATA_FRAME_SOF;
    // sof lrc
    m_netdata_frame_tx_buf.pre.lrc1 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.pre, offsetof(netdata_frame_preamble_t, lrc1));
    // cmd
    m_netdata_frame_tx_buf.pre.cmd = U16HTONS(cmd);
    // status
    m_netdata_frame_tx_buf.pre.status = U16HTONS(status);
    // data_length
    m_netdata_frame_tx_buf.pre.len = U16HTONS(data_length);
    // head lrc
    m_netdata_frame_tx_buf.pre.lrc2 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.pre, offsetof(netdata_frame_preamble_t, lrc2));
    // data
    if (data_length > 0) {
        memcpy(&m_netdata_frame_tx_buf.data, data, data_length);
    }
    // length out.
    m_frame_tx_buf_info.length = (sizeof(netdata_frame_preamble_t) + data_length + sizeof(netdata_frame_postamble_t));
    // data all lrc
    tx_post->lrc3 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.data, data_length);
    return (&m_frame_tx_buf_info);
}

/**
 * @brief Package receiving, which is used to receive the sent from the data packet and perform splicing processing
 * @param data: Receive byte array
 * @param length:The length of the receiving byte array
 */
void data_frame_receive(const uint8_t *data, uint16_t length) {
    data_frame_receive_from(DATA_SOURCE_USB, data, length);
}

void data_frame_receive_from(data_source_t source, const uint8_t *data, uint16_t length) {
    if (source >= DATA_SOURCE_COUNT || (!data && length)) return;
    receiver_t *rx = &receivers[source];
    // buffer wait process
    if (rx->complete || rx->processing) {
        NRF_LOG_ERROR("Data frame wait process.");
        return;
    }
    // buffer overflow
    if (rx->position + length > sizeof(rx->buffer)) {
        NRF_LOG_ERROR("Data frame wait overflow.");
        rx->position = 0;
        return;
    }
    // frame process
    for (int i = 0; i < length; i++) {
        // copy to buffer
        ((uint8_t *)(&rx->buffer))[rx->position] = data[i];
        if (rx->position == offsetof(netdata_frame_preamble_t, sof)) {
            if (rx->buffer.pre.sof != NETDATA_FRAME_SOF) {
                // not sof byte
                NRF_LOG_ERROR("Data frame no sof byte.");
                rx->position = 0;
                return;
            }
        } else if (rx->position == offsetof(netdata_frame_preamble_t, lrc1)) {
            if (rx->buffer.pre.lrc1 != compute_lrc((uint8_t *)&rx->buffer.pre, offsetof(netdata_frame_preamble_t, lrc1))) {
                // not sof lrc byte
                NRF_LOG_ERROR("Data frame sof lrc error.");
                rx->position = 0;
                return;
            }
        } else if (rx->position == offsetof(netdata_frame_preamble_t, lrc2)) {  // frame head lrc
            if (rx->buffer.pre.lrc2 != compute_lrc((uint8_t *)&rx->buffer.pre, offsetof(netdata_frame_preamble_t, lrc2))) {
                // frame head lrc error
                NRF_LOG_ERROR("Data frame head lrc error.");
                rx->position = 0;
                return;
            }
            // frame head complete, cache info
            rx->command = U16NTOHS(rx->buffer.pre.cmd);
            rx->status = U16NTOHS(rx->buffer.pre.status);
            rx->length = U16NTOHS(rx->buffer.pre.len);
            NRF_LOG_INFO("Data frame data length %d.", rx->length);
            // check data length
            if (rx->length > NETDATA_MAX_DATA_LENGTH) {
                NRF_LOG_ERROR("Data frame data length larger than max.");
                rx->position = 0;
                return;
            }
        } else if (rx->position >= offsetof(netdata_frame_raw_t, data)) {   // frame data
            // check all data ready.
            if (rx->position == (sizeof(netdata_frame_preamble_t) + rx->length)) {
                netdata_frame_postamble_t *rx_post = (netdata_frame_postamble_t *)((uint8_t *)&rx->buffer + sizeof(netdata_frame_preamble_t) + rx->length);
                if (rx_post->lrc3 == compute_lrc((uint8_t *)&rx->buffer.data, rx->length)) {
                    // ok, lrc for data is check success.
                    // and we are receive completed
                    rx->complete = true;
                    // NRF_LOG_INFO("RX Data frame: cmd = 0x%04x (%i), status = 0x%04x, length = %d%s", rx->command, rx->command, rx->status, rx->length, rx->length > 0 ? ", data =" : "");
                    // if (rx->length > 0) {
                    //     NRF_LOG_HEXDUMP_INFO(m_data_buffer, rx->length);
                    // }
                } else {
                    // data frame lrc error
                    NRF_LOG_ERROR("Data frame finally lrc error.");
                    rx->position = 0;
                }
                return;
            }
        }
        // index update
        rx->position++;
    }
}

/**
 * @brief After the data packet processing, when the received data forms a complete frame,
 *         This function will be distributed processing tasks through this function, which will be adjusted to notify the data processing of the data
 * If the data processing is time -consuming operation, you need to put this function in the main loop to call
 */
void data_frame_process(void) {
    for (data_source_t source = DATA_SOURCE_USB; source < DATA_SOURCE_COUNT; source++) {
        receiver_t *rx = &receivers[source];
        if (!rx->complete || (source == DATA_SOURCE_USB && !usb_cdc_tx_ready())) continue;
        uint32_t generation = generations[source];
        rx->processing = true;
        if (!rx->complete || generation != generations[source] ||
            (source == DATA_SOURCE_USB && !usb_cdc_tx_ready())) {
            rx->processing = false;
            continue;
        }
        processing_source = source;
        processing_generation = generation;
        if (m_frame_process_cbk) {
            m_frame_process_cbk(rx->command, rx->status, rx->length,
                rx->length ? rx->buffer.data : NULL);
        }
        rx->position = 0;
        rx->complete = false;
        rx->processing = false;
    }
}

/**
 * @brief Package processing registration registration
 */
void on_data_frame_complete(data_frame_cbk_t callback) {
    m_frame_process_cbk = callback;
}
