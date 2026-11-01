/* Exercise production descriptors and the RNDIS handler without USB hardware. */
#include "bytes.h"
#include "tusb.h"
#include "device/dcd.h"
#include "rndis_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern void rndis_class_set_handler(uint8_t *, int);
extern bool tud_vendor_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
char g_extern_product_str[]="Chameleon Ultra";
static unsigned notifications, transfers;
static _Alignas(uint32_t) uint8_t control[120];
static uint8_t ms_set[38];
static unsigned acknowledgements;
static bool mounted;
static bool accept_status = true;
bool tud_mounted(void) { return mounted; }
bool tud_control_status(uint8_t port, const tusb_control_request_t *request) {
    assert(port == 0 && request->wIndex == 8); acknowledgements++; return accept_status;
}

void netd_report(uint8_t *data, uint16_t size) {
    assert(size == 8 && data[0] == 1);
    notifications++;
}
bool tud_control_xfer(uint8_t port, const tusb_control_request_t *request, void *data,
                      uint16_t size) {
    assert(port == 0 && request->wIndex == 7 && size == sizeof(ms_set));
    memcpy(ms_set, data, size);
    transfers++;
    return true;
}

static void test_usb_descriptors(void) {
    const tusb_desc_device_t *device = (const void *)tud_descriptor_device_cb();
    assert(device->bNumConfigurations == 1 && device->idProduct == 0x8686 && device->idVendor == 0x6868);
    assert(device->bcdDevice == 0x0102 && device->bcdUSB == 0x0201);
    const uint8_t *bos = tud_descriptor_bos_cb();
    assert(bos[0] == 5 && bos[1] == TUSB_DESC_BOS && read_le16(bos + 2) == 33 && bos[4] == 1);
    assert(bos[5] == 28 && bos[6] == TUSB_DESC_DEVICE_CAPABILITY && bos[7] == DEVICE_CAPABILITY_PLATFORM);
    const uint8_t uuid[] = {TUD_BOS_MS_OS_20_UUID};
    assert(!memcmp(bos + 9, uuid, sizeof(uuid)) && read_le32(bos + 25) == 0x06030000);
    assert(read_le16(bos + 29) == sizeof(ms_set) && bos[31] == 0x20 && bos[32] == 1);
    tusb_control_request_t request = {.bmRequestType=0xc0,.bRequest=0x20,.wIndex=7,.wLength=sizeof(ms_set)};
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request) && transfers == 1);
    assert(read_le16(ms_set) == 10 && read_le16(ms_set + 2) == 0);
    assert(read_le32(ms_set + 4) == 0x06030000 && read_le16(ms_set + 8) == sizeof(ms_set));
    assert(read_le16(ms_set + 10) == 8 && read_le16(ms_set + 12) == 2 && ms_set[14] == 0);
    assert(read_le16(ms_set + 16) == 28 && read_le16(ms_set + 18) == 20 && read_le16(ms_set + 20) == 3);
    assert(!memcmp(ms_set + 22,"RNDIS\0\0\0",8) && !memcmp(ms_set + 30,"5162001\0",8));
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request) && transfers == 1);
    request.wValue=1;assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    request.wValue=0;request.bmRequestType=0x40;
    assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    request.wIndex=4;assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));

    for (unsigned mode = 0; mode < 2; mode++) {
        bool ecm = mode == 0;
        const uint8_t *config = tud_descriptor_configuration_cb(0);
        unsigned total=read_le16(config+2), interfaces=0, endpoints=0, associations=0;
        assert(config[4] == 4 && config[5] == 1);
        assert(total == TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + (ecm ? TUD_CDC_ECM_DESC_LEN : TUD_RNDIS_DESC_LEN));
        unsigned pos=0;int interface=-1;
        while (pos < total) {
            const uint8_t *d=config+pos;
            assert(d[0] >= 2 && d[0] <= total-pos);
            if (d[1] == TUSB_DESC_INTERFACE) {
                interfaces++;
                assert(d[2] == interface || d[2] == interface+1);interface=d[2];
                if (d[2] == 0) {
                    assert(d[5] == (ecm ? TUSB_CLASS_CDC : TUD_RNDIS_ITF_CLASS));
                    assert(d[6] == (ecm ? CDC_COMM_SUBCLASS_ETHERNET_CONTROL_MODEL : TUD_RNDIS_ITF_SUBCLASS));
                }
                if (d[2] == 2) assert(d[5] == TUSB_CLASS_CDC && d[6] == CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL);
            }
            if (d[1] == TUSB_DESC_INTERFACE_ASSOCIATION) {
                assert(d[2] == associations*2 && d[3] == 2);associations++;
            }
            if (d[1] == TUSB_DESC_ENDPOINT) {
                endpoints++;
                assert(read_le16(d+4) == (d[2] == 0x82 || (d[2] == 0x83 && !ecm) ? 8 : 64));
                if (interface < 2) assert(d[2] == 0x83 || d[2] == 0x04 || d[2] == 0x84);
                else assert(d[2] == 0x82 || d[2] == 0x01 || d[2] == 0x81);
            }
            pos+=d[0];
        }
        assert(pos == total && interfaces == (ecm ? 5 : 4) && endpoints == 6);
        assert(associations == 2 && interface == 3);
        assert(!tud_descriptor_configuration_cb(1) && !tud_descriptor_configuration_cb(255));
        if (ecm) {
            request=(tusb_control_request_t){.bmRequestType=0x40,.bRequest=0x20,.wIndex=8,.wValue=0x0100};
            request.wLength=1;assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
            request.wLength=0;request.wValue=1;assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
            request.wValue=0x0100;mounted=true;
            assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));mounted=false;
            assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request) && acknowledgements == 1);
            assert(tud_descriptor_configuration_cb(0) == config);
            assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request));
        }
    }
    tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true);
    assert(tud_descriptor_configuration_cb(0)[TUD_CONFIG_DESC_LEN+8+6] == CDC_COMM_SUBCLASS_ETHERNET_CONTROL_MODEL);
    assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request));
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true);
    assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request));
    accept_status=false;assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    assert(!tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request));accept_status=true;
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    assert(tud_descriptor_configuration_cb(0)[TUD_CONFIG_DESC_LEN+8+6] == CDC_COMM_SUBCLASS_ETHERNET_CONTROL_MODEL);
    /* An abandoned status stage leaves the default descriptor and can retry. */
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_SETUP,&request));
    assert(tud_vendor_control_xfer_cb(0,CONTROL_STAGE_ACK,&request));
    tud_event_hook_cb(0,DCD_EVENT_UNPLUGGED,true);
    assert(tud_descriptor_configuration_cb(0)[TUD_CONFIG_DESC_LEN+8+6] == CDC_COMM_SUBCLASS_ETHERNET_CONTROL_MODEL);
    const uint16_t *serial=tud_descriptor_string_cb(3,0);
    assert(serial[0] == 0x031a);
    for(unsigned j=0;j<12;j++) assert(serial[j+1] == "D23456789ABC"[j]);
    assert(!tud_descriptor_string_cb(6,0) && !tud_descriptor_string_cb(0xee,0));
}

static void prepare_message(uint32_t type, unsigned length) {
    memset(control, 0, sizeof(control));
    write_le32(control, type);
    write_le32(control + 4, length);
    write_le32(control + 8, 0x12345678);
}
static void expect_response(unsigned size, uint32_t type, unsigned length) {
    unsigned before = notifications;
    rndis_class_set_handler(control, size);
    assert(notifications == before + 1);
    assert(read_le32(control) == type && read_le32(control + 4) == length);
    if (type != REMOTE_NDIS_RESET_CMPLT)
        assert(read_le32(control + 8) == 0x12345678);
    assert(length <= sizeof(control));
}
static void expect_query(uint32_t oid, unsigned count, uint32_t status) {
    prepare_message(REMOTE_NDIS_QUERY_MSG, 28);
    write_le32(control + 12, oid);
    expect_response(28, REMOTE_NDIS_QUERY_CMPLT, 24 + count);
    assert(read_le32(control + 12) == status && read_le32(control + 16) == count);
}
static void test_rndis_control(void) {
    prepare_message(REMOTE_NDIS_INITIALIZE_MSG, 24);
    expect_response(24, REMOTE_NDIS_INITIALIZE_CMPLT, 52);
    assert(read_le32(control + 12) == RNDIS_STATUS_SUCCESS);
    assert(read_le32(control + 36) == CFG_TUD_NET_MTU + sizeof(rndis_data_packet_t));
    prepare_message(REMOTE_NDIS_QUERY_MSG, 28);
    write_le32(control + 12, OID_GEN_SUPPORTED_LIST);
    unsigned before_query = notifications;
    rndis_class_set_handler(control, 28);
    assert(notifications == before_query + 1);
    assert(read_le32(control) == REMOTE_NDIS_QUERY_CMPLT);
    assert(read_le32(control + 12) == RNDIS_STATUS_SUCCESS);
    unsigned oid_bytes = read_le32(control + 16);
    assert(oid_bytes > 0 && oid_bytes % 4 == 0 && oid_bytes <= sizeof(control) - 24);
    assert(read_le32(control + 4) == 24 + oid_bytes);
    uint8_t oids[sizeof(control) - 24];
    memcpy(oids, control + 24, oid_bytes);
    for (unsigned i = 0; i < oid_bytes; i += 4) {
        prepare_message(REMOTE_NDIS_QUERY_MSG, 28);
        write_le32(control + 12, read_le32(oids + i));
        unsigned before = notifications;
        rndis_class_set_handler(control, 28);
        assert(notifications == before + 1);
        assert(read_le32(control + 4) <= sizeof(control));
        assert(read_le32(control + 12) == RNDIS_STATUS_SUCCESS);
    }
    expect_query(OID_802_3_CURRENT_ADDRESS, 6, RNDIS_STATUS_SUCCESS);
    assert(!memcmp(control + 24, tud_network_mac_address, 6));
    expect_query(OID_802_3_PERMANENT_ADDRESS, 6, RNDIS_STATUS_SUCCESS);
    assert(!memcmp(control + 24, tud_network_mac_address, 6));
    prepare_message(REMOTE_NDIS_SET_MSG, 32);
    write_le32(control + 12, OID_GEN_CURRENT_PACKET_FILTER);
    write_le32(control + 16, 4);
    write_le32(control + 20, 20);
    write_le32(control + 28, 0x0f);
    expect_response(32, REMOTE_NDIS_SET_CMPLT, 16);
    assert(read_le32(control + 12) == RNDIS_STATUS_SUCCESS);
    expect_query(OID_GEN_CURRENT_PACKET_FILTER, 4, RNDIS_STATUS_SUCCESS);
    assert(read_le32(control + 24) == 0x0f);
    prepare_message(REMOTE_NDIS_KEEPALIVE_MSG, 12);
    expect_response(12, REMOTE_NDIS_KEEPALIVE_CMPLT, 16);
    prepare_message(REMOTE_NDIS_RESET_MSG, 12);
    expect_response(12, REMOTE_NDIS_RESET_CMPLT, 16);
    expect_query(OID_GEN_CURRENT_PACKET_FILTER, 4, RNDIS_STATUS_SUCCESS);
    assert(read_le32(control + 24) == 0);
    expect_query(0xffffffff, 0, RNDIS_STATUS_FAILURE);

    /* Unsupported vendor parameters must complete without parsing nested offsets. */
    prepare_message(REMOTE_NDIS_SET_MSG, 32);
    write_le32(control + 12, OID_GEN_RNDIS_CONFIG_PARAMETER);
    write_le32(control + 16, 4);
    write_le32(control + 20, 20);
    write_le32(control + 28, 0xffffffff);
    expect_response(32, REMOTE_NDIS_SET_CMPLT, 16);
    assert(read_le32(control + 12) == RNDIS_STATUS_FAILURE);

    /* HALT clears the filter without generating a completion. A truncated
     * HALT or RESET must leave the active filter unchanged.
     */
    prepare_message(REMOTE_NDIS_SET_MSG, 32);
    write_le32(control + 12, OID_GEN_CURRENT_PACKET_FILTER);
    write_le32(control + 16, 4);
    write_le32(control + 20, 20);
    write_le32(control + 28, 0x0f);
    expect_response(32, REMOTE_NDIS_SET_CMPLT, 16);
    for (unsigned i = 0; i < 2; i++) {
        unsigned before = notifications;
        prepare_message(i ? REMOTE_NDIS_RESET_MSG : REMOTE_NDIS_HALT_MSG, 8);
        rndis_class_set_handler(control, 8);
        assert(notifications == before && read_le32(control + 4) == 0);
        expect_query(OID_GEN_CURRENT_PACKET_FILTER, 4, RNDIS_STATUS_SUCCESS);
        assert(read_le32(control + 24) == 0x0f);
    }
    prepare_message(REMOTE_NDIS_HALT_MSG, 12);
    unsigned before_halt = notifications;
    rndis_class_set_handler(control, 12);
    assert(notifications == before_halt && read_le32(control + 4) == 0);
    expect_query(OID_GEN_CURRENT_PACKET_FILTER, 4, RNDIS_STATUS_SUCCESS);
    assert(read_le32(control + 24) == 0);

    unsigned before = notifications;
    for (unsigned size = 0; size < 28; size++) {
        prepare_message(REMOTE_NDIS_QUERY_MSG, 28);
        rndis_class_set_handler(control, size);
        assert(read_le32(control + 4) == 0);
    }
    for (unsigned i = 0; i < 4; i++) {
        prepare_message(REMOTE_NDIS_SET_MSG, 32);
        write_le32(control + 12, OID_GEN_CURRENT_PACKET_FILTER);
        write_le32(control + 16, i == 0 ? 0 : 4);
        write_le32(control + 20, i == 1 ? 0 : i == 2 ? 0xffffffff : 21);
        rndis_class_set_handler(control, 32);
        assert(read_le32(control + 4) == 0);
    }
    prepare_message(REMOTE_NDIS_QUERY_MSG, 121);
    rndis_class_set_handler(control, 121);
    prepare_message(REMOTE_NDIS_HALT_MSG, 12);
    rndis_class_set_handler(control, 12);
    assert(read_le32(control + 4) == 0 && notifications == before);
}

int main(void) {
    test_usb_descriptors();
    test_rndis_control();
    puts("USB descriptors and RNDIS control tests passed");
    return 0;
}
