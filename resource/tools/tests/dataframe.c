#include <assert.h>
#include <string.h>
#include "dataframe.h"
static unsigned calls;
static bool disconnect_in_callback;
static bool ready=true, reset_after_ready;
bool usb_cdc_tx_ready(void) {
    if (reset_after_ready) {
        reset_after_ready=false;
        data_frame_invalidate(DATA_SOURCE_USB);
        ready=false;
        return true;
    }
    return ready;
}
static void received(uint16_t command,uint16_t status,uint16_t length,uint8_t *data) {
    assert(status==0 && length==2);
    if (data_frame_source()==DATA_SOURCE_USB) { assert(command==1000 && data[0]==0xaa && data[1]==0xbb); }
    else { assert(command==1033 && data[0]==0xcc && data[1]==0xdd); }
    if (disconnect_in_callback) {
        uint8_t before[2];memcpy(before,data,2);
        data_frame_disconnect(data_frame_source());
        assert(!data_frame_reply_valid());
        assert(!memcmp(before,data,2));
        disconnect_in_callback=false;
    }
    calls++;
}
int main(void) {
    uint8_t usb[12],ble[12];
    data_frame_tx_t *frame=data_frame_make(1000,0,2,(uint8_t[]){0xaa,0xbb});
    assert(frame->length==12); memcpy(usb,frame->buffer,12);
    frame=data_frame_make(1033,0,2,(uint8_t[]){0xcc,0xdd});memcpy(ble,frame->buffer,12);
    assert(usb[0]==0x11 && usb[1]==0xef && usb[2]==3 && usb[3]==0xe8);
    on_data_frame_complete(received);
    data_frame_receive_from(DATA_SOURCE_USB,usb,4);
    data_frame_receive_from(DATA_SOURCE_BLE,ble,7);
    data_frame_receive_from(DATA_SOURCE_USB,usb+4,8);
    data_frame_receive_from(DATA_SOURCE_BLE,ble+7,5);
    data_frame_process();assert(calls==2);
    data_frame_receive_from(DATA_SOURCE_USB,usb,6);data_frame_disconnect(DATA_SOURCE_USB);
    data_frame_receive_from(DATA_SOURCE_BLE,ble,12);data_frame_process();assert(calls==3);
    data_frame_receive_from(DATA_SOURCE_USB,usb,12);data_frame_disconnect(DATA_SOURCE_USB);
    data_frame_process();assert(calls==3);
    usb[8]^=1;data_frame_receive_from(DATA_SOURCE_USB,usb,12);data_frame_process();assert(calls==3);
    usb[8]^=1;data_frame_receive_from(DATA_SOURCE_USB,usb,12);data_frame_process();assert(calls==4);
    disconnect_in_callback=true;
    data_frame_receive_from(DATA_SOURCE_USB,usb,12);data_frame_process();assert(calls==5);
    data_frame_receive_from(DATA_SOURCE_USB,usb,12);data_frame_process();assert(calls==6);
    data_frame_receive_from(DATA_SOURCE_USB,usb,12);reset_after_ready=true;
    data_frame_process();assert(calls==6);
    data_frame_disconnect(DATA_SOURCE_USB);ready=true;data_frame_process();assert(calls==6);
    assert(!data_frame_make(1000,0,4097,usb));
    return 0;
}
