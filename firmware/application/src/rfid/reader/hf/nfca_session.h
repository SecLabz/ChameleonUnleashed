#ifndef NFCA_SESSION_H
#define NFCA_SESSION_H

#include <stdbool.h>

/* Bounded NFC-A field lease shared by Classic, Type 2 and DESFire operations. */
void nfca_session_open(void);
/* Sector reads share the bounded RF lease, but never a capture's cipher state. */
void nfca_session_read_open(void);
bool nfca_session_reading(void);
void nfca_session_close(void);
bool nfca_session_active(void);
bool nfca_session_ready(void);
bool nfca_session_finish(void);
void nfca_session_poll(void);

#endif
