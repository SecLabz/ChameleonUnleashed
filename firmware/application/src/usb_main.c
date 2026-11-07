/* SPDX-License-Identifier: GPL-3.0-only */
#include "tusb.h"
#include "device/dcd.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
extern char g_extern_product_str[];
#ifdef WEB_RECOVERY
#include "nrf.h"
#include "net/network.h"
#endif
#if !defined(PLATFORM_HOST) && !defined(WEB_RECOVERY)
#include "nrf.h"
#include "hw_connect.h"
#include "usb_main.h"
#include "usb_events.h"
#include "usb_tearoff.h"
#include "dataframe.h"
#include "netdata.h"
#include "st25tb_tearoff.h"
#include "syssleep.h"
#include "nrf_drv_clock.h"
#include "nrf_drv_power.h"
#include "net/network.h"
extern void tusb_hal_nrf_power_event(uint32_t);
static void connection_end(void);
void USBD_IRQHandler(void) { tud_int_handler(0); }
#endif
uint8_t tud_network_mac_address[6] = {0x02, 0x43, 0x48, 0x46, 0x55, 0x4e};
enum {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_NETWORK_INTERFACE,
    STRING_HOST_MAC
};
enum { USB_PROFILE_ECM, USB_PROFILE_PENDING, USB_PROFILE_RNDIS };
static atomic_int usb_profile;
enum { ENDPOINT_NETWORK_NOTIFY = 0x83, ENDPOINT_NETWORK_OUT = 0x04, ENDPOINT_NETWORK_IN = 0x84 };
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0201,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x6868,
    .idProduct = 0x8686,
    .bcdDevice = 0x0102,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = 1};
uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

/* ECM is the default. Windows 10/11 requests RNDIS using MS OS 2.0 alternate
 * enumeration, before selecting configuration 1. Both profiles retain CDC. */
static const uint8_t rndis_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RNDIS_DESC_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED,
                          100),
    TUD_RNDIS_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, ENDPOINT_NETWORK_NOTIFY, 8,
                         ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE),
    TUD_CDC_DESCRIPTOR(2, 0, 0x82, 8, 0x01, 0x81, 64)};
static const uint8_t ecm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_CDC_ECM_DESC_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED,
                          100),
    TUD_CDC_ECM_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, STRING_HOST_MAC, ENDPOINT_NETWORK_NOTIFY,
                           64, ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE,
                           CFG_TUD_NET_MTU),
    TUD_CDC_DESCRIPTOR(2, 0, 0x82, 8, 0x01, 0x81, 64)};
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) {
    if (i) return NULL;
    return usb_profile == USB_PROFILE_RNDIS ? rndis_configuration : ecm_configuration;
}

enum { MS_OS_VENDOR_CODE = 0x20, MS_OS_DESCRIPTOR_INDEX = 7, MS_OS_ALT_ENUM_INDEX = 8 };
/* MS OS 2.0 set: one RNDIS function in the default configuration. */
/* clang-format off */
static const uint8_t ms_os_descriptor[] = {
    U16_TO_U8S_LE(10), U16_TO_U8S_LE(0), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(38),
    U16_TO_U8S_LE(8), U16_TO_U8S_LE(2), 0, 0, U16_TO_U8S_LE(28),
    U16_TO_U8S_LE(20), U16_TO_U8S_LE(3),
    'R', 'N', 'D', 'I', 'S', 0, 0, 0,
    '5', '1', '6', '2', '0', '0', '1', 0};
static const uint8_t bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_PLATFORM_DESCRIPTOR(TUD_BOS_MS_OS_20_UUID, U32_TO_U8S_LE(0x06030000),
        U16_TO_U8S_LE(sizeof(ms_os_descriptor)), MS_OS_VENDOR_CODE, 1)};
/* clang-format on */
_Static_assert(sizeof(ms_os_descriptor) == 38, "MS OS 2.0 descriptor set size");
uint8_t const *tud_descriptor_bos_cb(void) { return bos; }

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request) {
    if (request->bRequest != MS_OS_VENDOR_CODE) return false;
    if (request->bmRequestType == 0xc0 && request->wIndex == MS_OS_DESCRIPTOR_INDEX && !request->wValue) {
        return stage != CONTROL_STAGE_SETUP ||
            tud_control_xfer(rhport, request, (void *)ms_os_descriptor, sizeof(ms_os_descriptor));
    }
    if (request->bmRequestType != 0x40 || request->wIndex != MS_OS_ALT_ENUM_INDEX ||
        request->wValue != 0x0100 || request->wLength || tud_mounted()) return false;
    int expected = USB_PROFILE_PENDING;
    if (stage == CONTROL_STAGE_ACK) {
        return atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_RNDIS);
    }
    if (stage != CONTROL_STAGE_SETUP) return false;
    expected = usb_profile;
    if (expected == USB_PROFILE_RNDIS) return false;
    if (!atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_PENDING)) return false;
    if (tud_control_status(rhport, request)) return true;
    expected = USB_PROFILE_PENDING;
    atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_ECM);
    return false;
}

void tud_event_hook_cb(uint8_t rhport, uint32_t event, bool in_isr) {
    (void)rhport; (void)in_isr;
    if (event != DCD_EVENT_BUS_RESET && event != DCD_EVENT_UNPLUGGED) return;
    usb_profile = USB_PROFILE_ECM;
#if !defined(PLATFORM_HOST) && !defined(WEB_RECOVERY)
    connection_end();
#elif defined(WEB_RECOVERY)
    network_disconnect();
#endif
}

uint16_t const *tud_descriptor_string_cb(uint8_t i, uint16_t lang) {
    (void)lang;
    static uint16_t text[48];
    char serial[24], mac[13];
#ifdef PLATFORM_HOST
    uint32_t high = 0x1234, low = 0x56789abc;
#else
    uint32_t high = NRF_FICR->DEVICEADDR[1], low = NRF_FICR->DEVICEADDR[0];
#endif
    snprintf(serial, sizeof(serial), "%04X%08lX", (unsigned)((uint16_t)high | 0xC000),
             (unsigned long)low);
    for (unsigned j = 0; j < 6; j++)
        snprintf(mac + j * 2, 3, "%02X", tud_network_mac_address[j]);
    const char *strings[] = {[0] = "",
                             [STRING_MANUFACTURER] = "Proxgrind",
                             [STRING_PRODUCT] = g_extern_product_str,
                             [STRING_SERIAL] = serial,
                             [STRING_NETWORK_INTERFACE] = "Ultra USB",
                             [STRING_HOST_MAC] = mac};
    if (i == 0) {
        text[0] = (TUSB_DESC_STRING << 8) | 4;
        text[1] = 0x409;
        return text;
    }
    if (i >= sizeof(strings) / sizeof(*strings))
        return NULL;
    unsigned n = strlen(strings[i]);
    if (n > 47)
        n = 47;
    text[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    for (unsigned j = 0; j < n; j++)
        text[j + 1] = (uint8_t)strings[i][j];
    return text;
}


#if !defined(PLATFORM_HOST) && !defined(WEB_RECOVERY)
volatile bool g_usb_connected, g_usb_port_opened;
volatile bool g_usb_led_marquee_enable = true;
static uint8_t tx[sizeof(netdata_frame_raw_t)];
static uint16_t tx_size, tx_sent;
static atomic_bool reset_pending;
static uint8_t final_reply[ST25TB_TEAROFF_RESULT_BYTES + DATA_FRAME_OVERHEAD_BYTES];
static uint16_t final_length;
static bool final_expected;

static void connection_end(void) {
    g_usb_port_opened = false;
    usb_tearoff_cancel();
    data_frame_invalidate(DATA_SOURCE_USB);
    reset_pending = true;
    network_disconnect();
}

static void reset_transport(void) {
    if (!atomic_exchange(&reset_pending, false)) return;
    tx_size = tx_sent = final_length = 0;
    final_expected = false;
    usb_tearoff_reset_parser();
    data_frame_disconnect(DATA_SOURCE_USB);
    g_usb_led_marquee_enable = true;
}

void tud_mount_cb(void) { connection_end(); }

static void power_event(nrf_drv_power_usb_evt_t event) {
    tusb_hal_nrf_power_event(event);
    if (event == NRF_DRV_POWER_USB_EVT_READY) g_usb_connected = true;
    if (event == NRF_DRV_POWER_USB_EVT_REMOVED) {
        g_usb_connected = g_usb_port_opened = false;
        connection_end();
        sleep_timer_start(SLEEP_DELAY_MS_USB_POWER_DISCONNECTED);
    }
    if (event == NRF_DRV_POWER_USB_EVT_DETECTED) sleep_timer_stop();
}

void usb_init(void) {
    if (!nrf_drv_clock_init_check()) APP_ERROR_CHECK(nrf_drv_clock_init());
    if (!nrf_drv_power_init_check()) APP_ERROR_CHECK(nrf_drv_power_init(NULL));
}

void usb_start(void) {
    /* SoftDevice owns POWER_CLOCK; Nordic bridges its USB power events. */
    memcpy(tud_network_mac_address + 1, (const void *)NRF_FICR->DEVICEADDR, 5);
    NVIC_SetPriority(USBD_IRQn, 6);
    APP_ERROR_CHECK_BOOL(tud_init(0));
    const nrf_drv_power_usbevt_config_t config = { .handler = power_event };
    APP_ERROR_CHECK(nrf_drv_power_usbevt_init(&config));
    uint32_t status;
    APP_ERROR_CHECK(sd_power_usbregstatus_get(&status));
    if (status & POWER_USBREGSTATUS_VBUSDETECT_Msk) power_event(NRF_DRV_POWER_USB_EVT_DETECTED);
    if (status & POWER_USBREGSTATUS_OUTPUTRDY_Msk) power_event(NRF_DRV_POWER_USB_EVT_READY);
    APP_ERROR_CHECK_BOOL(network_init());
}

void tud_cdc_line_state_cb(uint8_t interface, bool dtr, bool rts) {
    (void)interface; (void)rts;
    g_usb_port_opened = dtr;
    usb_tearoff_reset_parser();
    if (!dtr) {
        usb_tearoff_cancel();
        tx_size = tx_sent = final_length = 0;
        final_expected = false;
        tud_cdc_read_flush();
        tud_cdc_write_clear();
        data_frame_disconnect(DATA_SOURCE_USB);
        g_usb_led_marquee_enable = true;
    }
}

void tud_umount_cb(void) {
    connection_end();
}

void usb_cdc_write(const void *data, uint16_t size) {
    if (!g_usb_port_opened || size > sizeof(tx)) return;
    bool final = final_expected;
    final_expected = false;
    if (tx_size) {
        if (final && size <= sizeof(final_reply)) {
            memcpy(final_reply, data, size);
            final_length = size;
        }
        return;
    }
    memcpy(tx, data, size); tx_size = size; tx_sent = 0;
}

bool is_usb_working(void) { return g_usb_port_opened; }

bool usb_hfclk_active(void) { return tud_mounted() && !tud_suspended(); }

bool usb_cdc_tx_ready(void) { return !reset_pending && !tx_size && !final_length; }

void usb_cdc_tearoff_end(void) {
    usb_tearoff_end();
    final_expected = true;
}

bool usb_work_pending(void) { return tud_task_event_ready() || usb_irq_work_pending(); }

void usb_poll(void) {
    reset_transport();
    tud_task();
    reset_transport();
    if (tx_size && tud_cdc_connected()) {
        tx_sent += tud_cdc_write(tx + tx_sent, tx_size - tx_sent);
        tud_cdc_write_flush();
        if (tx_sent == tx_size) tx_size = tx_sent = 0;
    }
    if (!tx_size && final_length && g_usb_port_opened) {
        memcpy(tx, final_reply, final_length);
        tx_size = final_length;
        tx_sent = final_length = 0;
    }
    if (g_usb_port_opened && (!tx_size || usb_cdc_tearoff_running())) {
        uint8_t bytes[64];
        while (tud_cdc_available()) {
            uint32_t size = tud_cdc_read(bytes, sizeof(bytes));
            if (!size) break;
            usb_tearoff_receive(bytes, size);
        }
    }
    network_poll();
}


#endif
