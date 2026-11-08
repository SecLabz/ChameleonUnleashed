#ifndef DATAFRAME_H
#define DATAFRAME_H

#include <stdint.h>
#include <stdbool.h>

/* Nine-byte frame header plus the final payload checksum. */
#define DATA_FRAME_OVERHEAD_BYTES 10u

typedef enum { DATA_SOURCE_USB, DATA_SOURCE_BLE, DATA_SOURCE_COUNT } data_source_t;

data_source_t data_frame_source(void);
bool data_frame_reply_valid(void);
void data_frame_receive_from(data_source_t source, const uint8_t *data, uint16_t length);
void data_frame_disconnect(data_source_t source);
bool data_frame_usb_pending(uint16_t command);
bool data_frame_usb_inside_frame(void);
// Interrupt-safe invalidation; the transport performs parser cleanup afterwards.
void data_frame_invalidate(data_source_t source);

// Data frame process callback
typedef void (*data_frame_cbk_t)(uint16_t cmd, uint16_t status, uint16_t length, uint8_t *data);

// TX buffer
typedef struct {
    uint8_t *const buffer;
    uint16_t length;
} data_frame_tx_t;

void data_frame_receive(const uint8_t *data, uint16_t length);
void data_frame_process(void);
/* Long web jobs service only their BLE control envelope, never legacy RF work. */
void data_frame_process_ble_control(void);
void on_data_frame_complete(data_frame_cbk_t callback);

data_frame_tx_t *data_frame_make(
    uint16_t cmd,
    uint16_t status,
    uint16_t length,
    uint8_t *data
);


#endif // DATAFRAME_H
