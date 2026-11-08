#ifndef BLE_MAIN_H
#define BLE_MAIN_H

#include "ble_bas.h"
#include "ble_gatts.h"
#include "ble_nus.h"
#include "nrfx_saadc.h"

extern uint16_t batt_lvl_in_milli_volts;
extern uint8_t percentage_batt_lvl;
extern volatile bool g_is_low_battery_shutdown;
extern volatile bool g_is_ble_connected;

typedef void (*lf_adc_callback_t)(nrf_saadc_value_t *, size_t);

void ble_slave_init(void);
bool ble_slave_rf_only(void);
void advertising_start(bool erase_bonds);
void advertising_stop(void);
bool advertising_stop_checked(void);
/* True when disconnected and advertising is stopped, including RF-only boots.
 * Resume only if this call actually stopped an advertising set. */
bool advertising_pause(bool *resume);
void delete_bonds_all(void);
void nus_data_response(uint8_t *p_data, uint16_t length);
uint16_t ble_nus_payload_limit(void);
/* Experimental link profiles: zero queries without changing the link.
 * version, connected, payload/interval/latency/timeout BE16, TX/RX PHY,
 * requested min/max BE16, GAP result BE32, PHY result BE32. */
#define BLE_LINK_INFO_BYTES 24u
void ble_link_profile(uint8_t profile, uint8_t phy, uint8_t output[BLE_LINK_INFO_BYTES]);
/* Queue one complete unsolicited job frame without interleaving API replies. */
bool ble_stream_response(uint8_t *data, uint16_t length);
bool is_nus_working(void);
void set_ble_connect_key(uint8_t *key);

void register_lf_adc_callback(lf_adc_callback_t cb);
void unregister_lf_adc_callback(void);

#endif
