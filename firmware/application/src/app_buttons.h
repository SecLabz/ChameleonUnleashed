#ifndef APP_BUTTONS_H
#define APP_BUTTONS_H

#include <stdbool.h>

/* Keep long RF callbacks out until button debounce and dispatch have finished. */
bool app_buttons_pending(void);

#endif
