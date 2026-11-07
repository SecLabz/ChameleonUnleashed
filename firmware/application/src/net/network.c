#include "network.h"
#include "http.h"
#include "dhcp.h"
#ifdef WEB_RECOVERY
#include "recovery.h"
#else
#include "app_timer.h"
#endif
#include "tusb.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <string.h>
extern uint8_t tud_network_mac_address[6];
static struct netif net;
static struct pbuf *incoming;
static volatile bool disconnected;
void network_disconnect(void) { disconnected = true; }
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t buffer[HTTP_REQUEST_MAX_BYTES];
    size_t received, sent;
    unsigned idle;
    bool responding, pending;
    http_response_t response;
} connection_t;
/* Browsers fetch up to six HTTP/1.1 resources in parallel. */
enum { HTTP_CONNECTIONS = 6 };
static connection_t connections[HTTP_CONNECTIONS];
/* Dispatch borrows request bytes; copy its result only after that borrow ends. */
static uint8_t response_scratch[HTTP_RESPONSE_MAX_BYTES];
static bool dispatching;
_Static_assert(HTTP_RESPONSE_MAX_BYTES <= HTTP_REQUEST_MAX_BYTES,
               "Response must fit the released request buffer");
u32_t sys_now(void) {
#ifdef WEB_RECOVERY
    return recovery_millis();
#else
    static uint32_t previous;
    static uint64_t ticks;
    uint32_t now = app_timer_cnt_get();
    ticks += app_timer_cnt_diff_compute(now, previous);
    previous = now;
    return (uint32_t)(ticks * 1000u * (APP_TIMER_CONFIG_RTC_FREQUENCY + 1u) / APP_TIMER_CLOCK_FREQ);
#endif
}

static void release(connection_t *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_poll(c->pcb, NULL, 0);
        tcp_err(c->pcb, NULL);
    }
    c->pcb = NULL;
}

static void error(void *arg, err_t err) {
    (void)err;
    if (arg)
        ((connection_t *)arg)->pcb = NULL;
}

static err_t flush(connection_t *c) {
    while (c->sent < c->response.header_size + c->response.size) {
        const void *p;
        size_t n;
        if (c->sent < c->response.header_size) {
            p = c->response.header + c->sent;
            n = c->response.header_size - c->sent;
        } else {
            size_t offset = c->sent - c->response.header_size;
            p = c->response.body + offset;
            n = c->response.size - offset;
        }
        if (n > tcp_sndbuf(c->pcb))
            n = tcp_sndbuf(c->pcb);
        if (!n)
            break;
        err_t err = tcp_write(c->pcb, p, n, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
            break;
        c->sent += n;
    }
    tcp_output(c->pcb);
    return ERR_OK;
}

static err_t sent(void *arg, struct tcp_pcb *pcb, u16_t n) {
    (void)n;
    connection_t *c = arg;
    c->idle = 0;
    if (c->sent == c->response.header_size + c->response.size && pcb->unacked == NULL &&
        pcb->unsent == NULL) {
        if (tcp_close(pcb) == ERR_OK) {
            release(c);
            return ERR_OK;
        }
    }
    return flush(c);
}

static err_t poll(void *arg, struct tcp_pcb *pcb) {
    connection_t *c = arg;
    if (++c->idle > 10) {
        release(c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return c->responding ? flush(c) : ERR_OK;
}

static err_t receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    connection_t *c = arg;
    if (!p) {
        release(c);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    if (c->responding || c->pending || p->tot_len > sizeof(c->buffer) - c->received) {
        pbuf_free(p);
        release(c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    pbuf_copy_partial(p, c->buffer + c->received, p->tot_len, 0);
    c->received += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    c->idle = 0;
    http_request_t request;
    int complete = http_parse_request(c->buffer, c->received, &request);
    if (complete) {
        c->pending = true;
        return ERR_OK;
    }
    return ERR_OK;
}

static err_t accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK)
        return err;
    for (unsigned i = 0; i < HTTP_CONNECTIONS; i++)
        if (!connections[i].pcb) {
            connection_t *c = &connections[i];
            memset(c, 0, sizeof(*c));
            c->pcb = pcb;
            tcp_arg(pcb, c);
            tcp_recv(pcb, receive);
            tcp_sent(pcb, sent);
            tcp_poll(pcb, poll, 2);
            tcp_err(pcb, error);
            return ERR_OK;
        }
    tcp_abort(pcb);
    return ERR_ABRT;
}

static err_t transmit(struct netif *n, struct pbuf *p) {
    (void)n;
    if (!tud_ready())
        return ERR_USE;
    if (!tud_network_can_xmit(p->tot_len))
        return ERR_MEM;
    tud_network_xmit(p, 0);
    return ERR_OK;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

bool tud_network_recv_cb(const uint8_t *p, uint16_t size) {
    /* TinyUSB renews RX when we decline the buffer. An accepted buffer would
     * require a later renew from network_poll(), which never sees this drop. */
    if (incoming || !size) return false;
    incoming = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!incoming)
        return false;
    if (pbuf_take(incoming, p, size) != ERR_OK) {
        pbuf_free(incoming);
        incoming = NULL;
        return false;
    }
    return true;
}

void tud_network_init_cb(void) {
    if (incoming) {
        pbuf_free(incoming);
        incoming = NULL;
    }
}

static err_t init(struct netif *n) {
    n->name[0] = 'f';
    n->name[1] = 'n';
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, tud_network_mac_address, 6);
    n->hwaddr[5] ^= 1;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    n->linkoutput = transmit;
    n->output = etharp_output;
    return ERR_OK;
}

bool network_init(void) {
    lwip_init();
    ip4_addr_t ip, mask, lease, gw;
    IP4_ADDR(&ip, 192, 168, 16, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&lease, 192, 168, 16, 2);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    if (!netif_add(&net, &ip, &mask, &gw, NULL, init, ethernet_input))
        return false;
    netif_set_default(&net);
    netif_set_up(&net);
    netif_set_link_up(&net);
    if (dhcp_server_init(&net, &ip, &mask, &lease) != ERR_OK)
        return false;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb || tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK)
        return false;
    pcb = tcp_listen(pcb);
    if (!pcb)
        return false;
    tcp_accept(pcb, accept);
    return true;
}

void network_poll(void) {
    if (disconnected) {
        disconnected = false;
        web_session_end();
        /* Include handshakes and closing PCBs no longer owned by HTTP. */
        while (tcp_active_pcbs)
            tcp_abort(tcp_active_pcbs);
        if (incoming) { pbuf_free(incoming); incoming = NULL; }
    }
    /* Retry segments and immediate ACKs after USB backpressure. */
    if (tud_ready() && tud_network_can_xmit(0)) {
        for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next)
            if (pcb->flags & (TF_NAGLEMEMERR | TF_ACK_NOW))
                tcp_output(pcb);
    }
    web_session_poll();
    for (unsigned i = 0; i < HTTP_CONNECTIONS; i++) {
        connection_t *c = &connections[i];
        if (c->pcb && c->responding && c->pcb->unsent)
            flush(c);
        if (!c->pcb || !c->pending || dispatching) continue;
        http_request_t request;
        http_parse_request(c->buffer, c->received, &request);
        c->pending = false;
        dispatching = true;
        c->response = http_dispatch_request(&request, response_scratch);
        if (c->response.body == response_scratch) {
            memcpy(c->buffer, response_scratch, c->response.size);
            c->response.body = c->buffer;
        }
        dispatching = false;
        c->responding = true;
        flush(c);
        break;
    }
    if (incoming) {
        struct pbuf *p = incoming;
        incoming = NULL;
        if (net.input(p, &net) != ERR_OK)
            pbuf_free(p);
        tud_network_recv_renew();
    }
    sys_check_timeouts();
}
