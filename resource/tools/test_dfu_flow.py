"""Native regressions for deferred Nordic DFU responses."""
from pathlib import Path
import unittest
from native_test import function, run_c

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / 'firmware/nrf52_sdk/components/libraries/bootloader'


class DfuFlowTests(unittest.TestCase):
    def test_create_waits_for_flash_and_execute_still_validates(self):
        source = (SDK / 'dfu/nrf_dfu_req_handler.c').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define UNUSED_RETURN_VALUE(x) (void)(x)
#define NRF_LOG_DEBUG(...) ((void)0)
#define NRF_SUCCESS 0
#define NRF_DFU_IN_APP 0
#define NRF_DFU_SAVE_PROGRESS_IN_FLASH 0
#define NRF_DFU_OP_OBJECT_CREATE 1
#define NRF_DFU_OP_OBJECT_WRITE 8
#define NRF_DFU_OP_CRC_GET 3
#define NRF_DFU_OP_OBJECT_EXECUTE 4
#define NRF_DFU_OP_OBJECT_SELECT 6
#define NRF_DFU_RES_CODE_SUCCESS 1
#define NRF_DFU_RES_CODE_OPERATION_FAILED 10
#define ASSERT assert
typedef unsigned ret_code_t;
typedef void (*nrf_dfu_flash_callback_t)(void *);
typedef struct {unsigned request,result;} nrf_dfu_response_t;
typedef struct {unsigned request;void *p_context;struct {void(*response)(nrf_dfu_response_t *,void *);}callback;} nrf_dfu_request_t;
static bool busy,create_busy,queue_error;
static unsigned responses,last_result,creation_error,validations,saves;
static void *last_context;
static nrf_dfu_request_t queued;
static void(*scheduled)(void *,uint16_t);
static struct {struct {unsigned firmware_image_offset;}progress;} s_dfu_settings;
static unsigned m_firmware_size_req=100,m_firmware_start_addr=0x27000;
bool nrf_fstorage_is_busy(void *p){(void)p;return busy;}
unsigned app_sched_event_put(void *p,unsigned size,void(*cb)(void *,uint16_t)){
 assert(size==sizeof(queued));if(queue_error)return 1;queued=*(nrf_dfu_request_t *)p;scheduled=cb;return 0;
}
void reply(nrf_dfu_response_t *r,void *ctx){responses++;last_result=r->result;last_context=ctx;}
unsigned nrf_dfu_validation_activation_prepare(unsigned addr,unsigned size){assert(addr==0x27000 && size==100);validations++;return 1;}
unsigned ext_err_code_handle(unsigned result){return result;}
void on_dfu_complete(void *p){(void)p;}
unsigned nrf_dfu_settings_write_and_backup(nrf_dfu_flash_callback_t cb){(void)cb;saves++;return 0;}
void on_data_obj_create_request(nrf_dfu_request_t *r,nrf_dfu_response_t *s){(void)r;s->result=creation_error?creation_error:1;busy=create_busy;}
void on_data_obj_write_request(nrf_dfu_request_t *r,nrf_dfu_response_t *s){(void)r;(void)s;}
void on_data_obj_crc_request(nrf_dfu_request_t *r,nrf_dfu_response_t *s){(void)r;(void)s;}
void on_data_obj_select_request(nrf_dfu_request_t *r,nrf_dfu_response_t *s){(void)r;(void)s;}
bool on_data_obj_execute_request(nrf_dfu_request_t *r,nrf_dfu_response_t *s){(void)r;(void)s;return true;}
''' + function(source, 'on_data_obj_response_sched') + '\n' + function(source, 'nrf_dfu_data_req') + r'''
static void drain(void){assert(scheduled);nrf_dfu_request_t event=queued;void(*cb)(void *,uint16_t)=scheduled;scheduled=NULL;cb(&event,sizeof(event));}
int main(void){
 int identity;
 nrf_dfu_request_t req={.request=1,.p_context=&identity,.callback.response=reply};
 nrf_dfu_response_t res={.request=1,.result=1};
 create_busy=true;
 assert(!nrf_dfu_data_req(&req,&res) && !responses && scheduled);
 req.p_context=NULL;drain();assert(!responses && scheduled); /* Scheduler owns a request copy. */
 busy=false;drain();assert(responses==1 && last_result==1 && last_context==&identity && !scheduled);
 create_busy=false;assert(!nrf_dfu_data_req(&req,&res) && responses==2 && !scheduled); /* Synchronous USB backend. */
 creation_error=3;assert(nrf_dfu_data_req(&req,&res) && res.result==3 && responses==2 && !scheduled);
 creation_error=0;create_busy=true;queue_error=true;
 assert(!nrf_dfu_data_req(&req,&res) && responses==3 && last_result==10 && !scheduled);
 queue_error=false;busy=false;req.request=4;
 s_dfu_settings.progress.firmware_image_offset=100;
 on_data_obj_response_sched(&req,0);assert(validations==1 && saves==1 && responses==4);
}
''')

    def test_receipts_wait_for_writes_and_cannot_cross_connections(self):
        source = (SDK / 'ble_dfu/nrf_dfu_ble.c').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define NRF_SUCCESS 0
#define BLE_CONN_HANDLE_INVALID 0xffff
#define NRF_DFU_RES_CODE_OPERATION_FAILED 10
static unsigned m_connection_id=1,m_conn_handle=0;
typedef struct {uint32_t connection_id;uint8_t response[11];} dfu_receipt_t;
static dfu_receipt_t queued;
static void(*scheduled)(void *,uint16_t);
static bool busy,queue_error;
static unsigned sent,status;
bool nrf_fstorage_is_busy(void *p){(void)p;return busy;}
unsigned app_sched_event_put(void *p,unsigned size,void(*cb)(void *,uint16_t)){
 assert(size==sizeof(queued));if(queue_error)return 1;queued=*(dfu_receipt_t *)p;scheduled=cb;return 0;
}
unsigned response_send(uint8_t *p,unsigned size){assert(size==11);sent++;status=p[2];return 0;}
''' + function(source, 'receipt_send') + r'''
static void drain(void){assert(scheduled);dfu_receipt_t event=queued;void(*cb)(void *,uint16_t)=scheduled;scheduled=NULL;cb(&event,sizeof(event));}
int main(void){
 dfu_receipt_t receipt={.connection_id=1,.response={0x60,3,1}};
 busy=true;receipt_send(&receipt,sizeof(receipt));assert(!sent && scheduled);
 receipt.response[2]=8;drain();assert(!sent && scheduled);
 busy=false;drain();assert(sent==1 && status==1 && !scheduled); /* Snapshot survived the caller. */
 busy=true;receipt_send(&receipt,sizeof(receipt));m_connection_id++;busy=false;drain();assert(sent==1);
 receipt.connection_id=m_connection_id;m_conn_handle=BLE_CONN_HANDLE_INVALID;
 receipt_send(&receipt,sizeof(receipt));assert(sent==1);
 m_conn_handle=0;busy=true;queue_error=true;
 receipt_send(&receipt,sizeof(receipt));assert(sent==2 && status==10 && !scheduled);
}
''')
