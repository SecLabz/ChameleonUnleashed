/* Host hardware stubs; Python inserts the unchanged production callbacks. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "usb_main.h"
#include "usb_tearoff.h"

#define DATA_SOURCE_USB 0
#define DATA_SOURCE_BLE 1
typedef struct { uint8_t *const buffer; uint16_t length; } data_frame_tx_t;
#define DCD_EVENT_BUS_RESET 1
#define DCD_EVENT_UNPLUGGED 2
#define NRF_SUCCESS 0
#define NRF_ERROR_INVALID_STATE 1
#define NRF_ERROR_NOT_FOUND 2
#define NRF_ERROR_BUSY 3
#define NRF_ERROR_RESOURCES 4
#define MIN(a,b) ((a)<(b)?(a):(b))
#define APP_TIMER_TICKS(ms) (ms)
#define APP_ERROR_CHECK(e) assert((e)==0)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
#define NRF_LOG_HEXDUMP_DEBUG(...) ((void)0)
typedef unsigned ret_code_t;
static bool g_usb_connected, g_usb_port_opened, g_usb_led_marquee_enable;
static uint8_t tx[4106];
static uint16_t tx_size, tx_sent;
static atomic_bool reset_pending;
static uint8_t final_reply[74];
static uint16_t final_length;
static bool final_expected;
enum { USB_PROFILE_ECM, USB_PROFILE_PENDING, USB_PROFILE_RNDIS };
static atomic_int usb_profile;
static unsigned invalidated, cleaned, network_ends, rx_flushed, tx_flushed;
static unsigned usb_queued, incoming;
static void data_frame_invalidate(int source) {(void)source;invalidated++;}
static void data_frame_disconnect(int source) {(void)source;cleaned++;}
static void network_disconnect(void) {network_ends++;}
static void network_poll(void) {}
static void tud_task(void) {}
static bool tud_cdc_connected(void) {return g_usb_port_opened;}
static unsigned tud_cdc_write(const void *data, unsigned size) {(void)data;usb_queued+=size;return size;}
static void tud_cdc_write_flush(void) {}
static unsigned tud_cdc_available(void) {return incoming;}
static unsigned tud_cdc_read(void *data, unsigned size) {(void)data;(void)size;unsigned n=incoming;incoming=0;return n;}
static void tud_cdc_read_flush(void) {incoming=0;rx_flushed++;}
static void tud_cdc_write_clear(void) {usb_queued=0;tx_flushed++;}
static bool inside, pending;
static uint8_t received[128];
static unsigned received_length;
bool data_frame_usb_inside_frame(void) { return inside; }
bool data_frame_usb_pending(uint16_t command) { assert(command==2310); return pending; }
void data_frame_receive(const uint8_t *data,uint16_t size) {
    assert(received_length+size<=sizeof(received));
    memcpy(received+received_length,data,size);received_length+=size;
}

static uint16_t m_conn_handle=7, m_ble_nus_max_data_len=20;
static unsigned m_nus, sends, feeds, ticks, send_error;
static bool g_is_ble_connected=true, reply_valid=true;
static int response_source=DATA_SOURCE_USB;
static int data_frame_source(void) { return response_source; }
bool is_usb_working(void) { return g_usb_port_opened; }
static bool is_nus_working(void) { return false; }
static bool data_frame_reply_valid(void) {return reply_valid;}
static unsigned app_timer_cnt_get(void) {return ticks++;}
static unsigned app_timer_cnt_diff_compute(unsigned now,unsigned then) {return now-then;}
static void bsp_wdt_feed(void) {feeds++;}
static ret_code_t ble_nus_data_send(void *nus,uint8_t *data,uint16_t *size,uint16_t connection) {
    (void)nus;(void)data;assert(connection==7 && *size<=20);sends++;return send_error;
}

/* PRODUCTION_CALLBACKS */

int main(void) {
    const uint8_t bytes[]={1,2,3};
    g_usb_connected=g_usb_port_opened=true;
    usb_cdc_write(bytes,sizeof(bytes));
    tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true);
    assert(invalidated==1 && network_ends==1 && !usb_cdc_tx_ready());
    assert(tx_size==3 && cleaned==0); /* No foreground state mutation in IRQ. */
    usb_poll();
    assert(cleaned==1 && tx_size==0 && usb_queued==0 && usb_cdc_tx_ready());
    tud_mount_cb();assert(!usb_cdc_tx_ready());usb_poll();
    tud_cdc_line_state_cb(0,true,false);
    usb_cdc_write(bytes,sizeof(bytes));usb_poll();assert(usb_queued==3);
    incoming=8;
    tud_cdc_line_state_cb(0,false,false);
    assert(!g_usb_port_opened && !incoming && !usb_queued && rx_flushed==1 && tx_flushed==1);
    tud_cdc_line_state_cb(0,true,false);usb_poll();assert(!usb_queued);
    tud_umount_cb();assert(!usb_cdc_tx_ready());usb_poll();assert(!g_usb_port_opened);
    uint8_t response[4096]={0};
    send_error=NRF_ERROR_INVALID_STATE;nus_data_response(response,sizeof(response));assert(sends==1);
    sends=0;send_error=NRF_ERROR_NOT_FOUND;nus_data_response(response,sizeof(response));assert(sends==1);
    sends=0;send_error=NRF_ERROR_RESOURCES;nus_data_response(response,sizeof(response));assert(sends<=251);
    sends=0;send_error=NRF_ERROR_BUSY;nus_data_response(response,sizeof(response));assert(sends<=251);
    sends=0;feeds=0;send_error=0;nus_data_response(response,sizeof(response));assert(sends==205 && feeds==206);
    /* A final reply must follow a blocked progress frame, never disappear. */
    g_usb_port_opened=true;
    usb_cdc_tearoff_begin();
    usb_cdc_write(bytes,sizeof(bytes));
    usb_cdc_tearoff_end();
    uint8_t final[74]={0};
    usb_cdc_write(final,sizeof(final));
    assert(final_length==74 && !usb_cdc_tx_ready());
    usb_poll();
    assert(tx_size==74 && !final_length && !usb_cdc_tx_ready());
    usb_poll();assert(usb_cdc_tx_ready());
    /* Fragmented cancellation works while framed payload bytes are preserved. */
    const uint8_t marker[]={3,'S','T','2','5','S','T','O','P'};
    usb_cdc_tearoff_begin();
    for(unsigned i=0;i<sizeof(marker);i++)usb_tearoff_receive(marker+i,1);
    assert(usb_cdc_tearoff_cancelled() && !received_length);
    usb_cdc_request_end();inside=true;
    usb_tearoff_receive(marker,sizeof(marker));
    assert(!usb_cdc_tearoff_cancelled() && received_length==sizeof(marker) && !memcmp(received,marker,sizeof(marker)));
    inside=false;received_length=0;
    usb_tearoff_receive(marker,3);
    const uint8_t mismatch='X';usb_tearoff_receive(&mismatch,1);
    assert(received_length==4 && !memcmp(received,marker,3) && received[3]=='X');
    tud_cdc_line_state_cb(0,false,false);
    assert(usb_cdc_tearoff_cancelled() && !final_length);
    usb_tearoff_end();
    pending=true;usb_cdc_request_end();
    usb_tearoff_receive(marker,sizeof(marker));assert(usb_cdc_tearoff_cancelled());
    uint8_t rejected[10]={0};
    data_frame_tx_t rejected_response={.buffer=rejected,.length=sizeof(rejected)};
    response_source=DATA_SOURCE_BLE;auto_response_data(&rejected_response);assert(usb_cdc_tearoff_cancelled());
    response_source=DATA_SOURCE_USB;auto_response_data(&rejected_response);assert(!usb_cdc_tearoff_cancelled());
    pending=false;usb_cdc_tearoff_begin();assert(!usb_cdc_tearoff_cancelled());
    usb_cdc_tearoff_end();
    usb_cdc_tearoff_begin();
    tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true);
    assert(reset_pending);
    assert(usb_cdc_tearoff_cancelled() && !reset_pending);
    usb_poll();assert(usb_cdc_tearoff_cancelled() && usb_cdc_tearoff_running());
    usb_cdc_tearoff_end();usb_cdc_request_end();assert(!usb_cdc_tearoff_cancelled());
    reply_valid=false;sends=0;nus_data_response(response,sizeof(response));assert(!sends);
    return 0;
}
