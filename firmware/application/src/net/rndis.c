/*
  The original version of this code was lrndis/usbd_rndis_core.c from
  https://github.com/fetisov/lrndis It has since been overhauled to suit this application
*/

/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 by Sergey Fetisov <fsenok@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Adapted from TinyUSB 0.17.0 lib/networking/rndis_reports.c.
 * Only control messages are handled here; TinyUSB owns Ethernet frame transport.
 */
#include "bytes.h"
#include <string.h>
#include "class/net/net_device.h"
#include "rndis_protocol.h"
#include "netif/ethernet.h"

#define RNDIS_LINK_SPEED 12000000 /* Link baudrate (12Mbit/s for USB-FS) */

/* OIDs and the ECM MAC string both describe the host adapter. The lwIP
 * device address differs in its final bit (see network.c).
 */
static uint32_t packet_filter;
static const char vendor_description[] = "Chameleon Ultra";

CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t response_available_notice[8] = {1};

static const uint32_t supported_oids[] = {
    OID_GEN_SUPPORTED_LIST,       OID_GEN_HARDWARE_STATUS,       OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,         OID_GEN_MAXIMUM_FRAME_SIZE,    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BLOCK_SIZE,  OID_GEN_RECEIVE_BLOCK_SIZE,    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,   OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_MAXIMUM_TOTAL_SIZE,   OID_GEN_PROTOCOL_OPTIONS,      OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,  OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,    OID_802_3_MULTICAST_LIST,      OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_MAC_OPTIONS};

/* TinyUSB 0.17.0 ecm_rndis_device.c owns a 120-byte control buffer. */
enum { RNDIS_CONTROL_SIZE = 120 };
_Static_assert(sizeof(supported_oids) + sizeof(rndis_query_cmplt_t) <= RNDIS_CONTROL_SIZE,
               "RNDIS supported OIDs must fit TinyUSB's control buffer");

static void notify_response_available(void) {
    netd_report(response_available_notice, sizeof(response_available_notice));
}

static void send_query_response(uint8_t *buffer, uint32_t status, const void *payload,
                                size_t payload_size) {
    rndis_query_cmplt_t *response = (void *)buffer;
    response->MessageType = REMOTE_NDIS_QUERY_CMPLT;
    response->MessageLength = sizeof(*response) + payload_size;
    response->InformationBufferLength = payload_size;
    response->InformationBufferOffset =
        sizeof(*response) - offsetof(rndis_query_cmplt_t, RequestId);
    response->Status = status;
    if (payload_size)
        memcpy(response + 1, payload, payload_size);
    notify_response_available();
}

static void send_query_u32_response(uint8_t *buffer, uint32_t value) {
    send_query_response(buffer, RNDIS_STATUS_SUCCESS, &value, sizeof(value));
}

static void handle_query(uint8_t *buffer) {
    switch (read_le32(buffer + offsetof(rndis_query_msg_t, Oid))) {
    case OID_GEN_SUPPORTED_LIST:
        send_query_response(buffer, RNDIS_STATUS_SUCCESS, supported_oids, sizeof(supported_oids));
        return;
    case OID_GEN_VENDOR_DRIVER_VERSION:
        send_query_u32_response(buffer, 0x00001000);
        return;
    case OID_802_3_CURRENT_ADDRESS:
    case OID_802_3_PERMANENT_ADDRESS:
        send_query_response(buffer, RNDIS_STATUS_SUCCESS, tud_network_mac_address,
                            sizeof(tud_network_mac_address));
        return;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
    case OID_GEN_PHYSICAL_MEDIUM:
        send_query_u32_response(buffer, NDIS_MEDIUM_802_3);
        return;
    case OID_GEN_LINK_SPEED:
        send_query_u32_response(buffer, RNDIS_LINK_SPEED / 100);
        return;
    case OID_GEN_VENDOR_ID:
        send_query_u32_response(buffer, 0x00FFFFFF);
        return;
    case OID_GEN_VENDOR_DESCRIPTION:
        send_query_response(buffer, RNDIS_STATUS_SUCCESS, vendor_description,
                            sizeof(vendor_description));
        return;
    case OID_GEN_CURRENT_PACKET_FILTER:
        send_query_u32_response(buffer, packet_filter);
        return;
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        send_query_u32_response(buffer, CFG_TUD_NET_MTU - SIZEOF_ETH_HDR);
        return;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        send_query_u32_response(buffer, CFG_TUD_NET_MTU);
        return;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        send_query_u32_response(buffer, NDIS_MEDIA_STATE_CONNECTED);
        return;
    case OID_GEN_MAXIMUM_SEND_PACKETS:
    case OID_802_3_MAXIMUM_LIST_SIZE:
        send_query_u32_response(buffer, 1);
        return;
    case OID_802_3_MULTICAST_LIST:
        send_query_response(buffer, RNDIS_STATUS_SUCCESS, NULL, 0);
        return;
    case OID_GEN_HARDWARE_STATUS:
    case OID_GEN_PROTOCOL_OPTIONS:
    case OID_GEN_MAC_OPTIONS:
    case OID_GEN_RNDIS_CONFIG_PARAMETER:
    case OID_802_3_MAC_OPTIONS:
    /* Transport statistics are not tracked by this control handler. */
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS:
    case OID_GEN_XMIT_OK:
    case OID_GEN_RCV_OK:
    case OID_GEN_RCV_ERROR:
    case OID_GEN_XMIT_ERROR:
    case OID_GEN_RCV_NO_BUFFER:
        send_query_u32_response(buffer, 0);
        return;
    default:
        send_query_response(buffer, RNDIS_STATUS_FAILURE, NULL, 0);
        return;
    }
}

static void handle_set(uint8_t *buffer) {
    uint32_t oid = read_le32(buffer + offsetof(rndis_set_msg_t, Oid));
    rndis_set_cmplt_t *response = (void *)buffer;
    response->MessageType = REMOTE_NDIS_SET_CMPLT;
    response->MessageLength = sizeof(*response);
    response->Status = RNDIS_STATUS_SUCCESS;

    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER: {
        /* The dispatcher has validated this four-byte information buffer. */
        uint32_t offset = read_le32(buffer + offsetof(rndis_set_msg_t, InformationBufferOffset));
        packet_filter = read_le32(buffer + offsetof(rndis_set_msg_t, RequestId) + offset);
        break;
    }
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_PROTOCOL_OPTIONS:
    case OID_802_3_MULTICAST_LIST:
        /* Accepted for the point-to-point link; no hardware filtering is needed. */
        break;
    default:
        response->Status = RNDIS_STATUS_FAILURE;
        break;
    }
    notify_response_available();
}

void rndis_class_set_handler(uint8_t *buffer, int received_size) {
    /* TinyUSB always supplies its 120-byte buffer; received_size is the OUT
     * request length. Clear stale responses even for truncated messages.
     */
    uint32_t message_type = read_le32(buffer);
    uint32_t message_size = read_le32(buffer + 4);
    write_le32(buffer + 4, 0);
    if (received_size < 8 || received_size > RNDIS_CONTROL_SIZE ||
        message_size > (unsigned)received_size)
        return;

    unsigned minimum;
    switch (message_type) {
    case REMOTE_NDIS_INITIALIZE_MSG:
        minimum = sizeof(rndis_initialize_msg_t);
        break;
    case REMOTE_NDIS_QUERY_MSG:
    case REMOTE_NDIS_SET_MSG:
        minimum = sizeof(rndis_query_msg_t);
        break;
    case REMOTE_NDIS_RESET_MSG:
        minimum = sizeof(rndis_reset_msg_t);
        break;
    case REMOTE_NDIS_KEEPALIVE_MSG:
        minimum = sizeof(rndis_keepalive_msg_t);
        break;
    case REMOTE_NDIS_HALT_MSG:
        minimum = sizeof(rndis_halt_msg_t);
        break;
    default:
        return;
    }
    if (message_size < minimum)
        return;
    if (message_type == REMOTE_NDIS_QUERY_MSG || message_type == REMOTE_NDIS_SET_MSG) {
        uint32_t information_size =
            read_le32(buffer + offsetof(rndis_query_msg_t, InformationBufferLength));
        uint32_t information_offset =
            read_le32(buffer + offsetof(rndis_query_msg_t, InformationBufferOffset));
        const unsigned information_base = offsetof(rndis_query_msg_t, RequestId);
        if (information_size &&
            (information_offset < sizeof(rndis_query_msg_t) - information_base ||
             information_offset > message_size - information_base ||
             information_size > message_size - information_base - information_offset))
            return;
        if (message_type == REMOTE_NDIS_SET_MSG &&
            read_le32(buffer + offsetof(rndis_set_msg_t, Oid)) == OID_GEN_CURRENT_PACKET_FILTER &&
            information_size != sizeof(packet_filter))
            return;
    }
    if (message_type == REMOTE_NDIS_INITIALIZE_MSG || message_type == REMOTE_NDIS_RESET_MSG)
        packet_filter = 0;
    switch (message_type) {
    case REMOTE_NDIS_INITIALIZE_MSG: {
        rndis_initialize_cmplt_t *response = (void *)buffer;
        /* RequestId is preserved in the shared request/response buffer. */
        response->MessageType = REMOTE_NDIS_INITIALIZE_CMPLT;
        response->MessageLength = sizeof(rndis_initialize_cmplt_t);
        response->MajorVersion = RNDIS_MAJOR_VERSION;
        response->MinorVersion = RNDIS_MINOR_VERSION;
        response->Status = RNDIS_STATUS_SUCCESS;
        response->DeviceFlags = RNDIS_DF_CONNECTIONLESS;
        response->Medium = RNDIS_MEDIUM_802_3;
        response->MaxPacketsPerTransfer = 1;
        response->MaxTransferSize = CFG_TUD_NET_MTU + sizeof(rndis_data_packet_t);
        response->PacketAlignmentFactor = 0;
        response->AfListOffset = 0;
        response->AfListSize = 0;
        notify_response_available();
    } break;

    case REMOTE_NDIS_QUERY_MSG:
        handle_query(buffer);
        break;

    case REMOTE_NDIS_SET_MSG:
        handle_set(buffer);
        break;

    case REMOTE_NDIS_RESET_MSG: {
        rndis_reset_cmplt_t *response = (void *)buffer;
        response->MessageType = REMOTE_NDIS_RESET_CMPLT;
        response->MessageLength = sizeof(rndis_reset_cmplt_t);
        response->Status = RNDIS_STATUS_SUCCESS;
        response->AddressingReset = 1; /* Ask the host to restore its cleared packet filter. */
        notify_response_available();
    } break;

    case REMOTE_NDIS_KEEPALIVE_MSG: {
        rndis_keepalive_cmplt_t *response = (void *)buffer;
        response->MessageType = REMOTE_NDIS_KEEPALIVE_CMPLT;
        response->MessageLength = sizeof(rndis_keepalive_cmplt_t);
        response->Status = RNDIS_STATUS_SUCCESS;
        notify_response_available();
    } break;

    case REMOTE_NDIS_HALT_MSG:
        packet_filter = 0;
        break;
    default:
        break;
    }
}
