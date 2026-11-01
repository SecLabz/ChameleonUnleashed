/* Exercise the production network loop with real lwIP and a busy USB IN slot. */
#include "net/network.c"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include <assert.h>

uint8_t tud_network_mac_address[6] = {2, 0, 0, 0, 0, 1};
static bool ready = true, writable;
static unsigned packets, ended;
static uint8_t packet[1600];
static uint16_t packet_size;
static unsigned dispatched;
static bool nested_dispatch, asset_response;
static unsigned renewals;
static const uint8_t asset[1024] = {0x73};

uint32_t app_timer_cnt_get(void) { return 0; }
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) { return now - before; }
bool tud_ready(void) { return ready; }
bool tud_network_can_xmit(uint16_t size) { assert(size <= sizeof(packet)); return writable; }
void tud_network_xmit(void *ref, uint16_t arg) {
    assert(writable);
    packet_size = tud_network_xmit_cb(packet, ref, arg);
    writable = false;
    packets++;
}
void tud_network_recv_renew(void) { renewals++; }
void web_session_end(void) { ended++; }
void web_session_poll(void) {}
err_t dhcp_server_init(struct netif *n, const ip4_addr_t *ip, const ip4_addr_t *mask,
                   const ip4_addr_t *lease) {
    (void)n; (void)ip; (void)mask; (void)lease;
    return ERR_OK;
}
int http_parse_request(const uint8_t *bytes, size_t size, http_request_t *request) {
    *request = (http_request_t){.bytes = bytes, .size = size};
    return size == HTTP_REQUEST_MAX_BYTES;
}
http_response_t http_dispatch_request(const http_request_t *request, uint8_t *output) {
    assert(request->size == HTTP_REQUEST_MAX_BYTES);
    uint8_t value = request->bytes[0];
    for (size_t i = 0; i < request->size; i++) assert(request->bytes[i] == value);
    memset(output, value + 10, HTTP_RESPONSE_MAX_BYTES);
    dispatched++;
    if (nested_dispatch) {
        nested_dispatch = false;
        unsigned before = dispatched;
        network_poll();
        assert(dispatched == before);
        for (size_t i = 0; i < request->size; i++) assert(request->bytes[i] == value);
    }
    return (http_response_t){.status = 200,
        .body = asset_response ? asset : output,
        .size = asset_response ? sizeof(asset) : HTTP_RESPONSE_MAX_BYTES};
}

/* Skip ARP in these TCP progress checks; the production USB output is retained. */
static err_t output(struct netif *n, struct pbuf *p, const ip4_addr_t *ip) {
    (void)ip;
    return n->linkoutput(n, p);
}

static struct tcp_pcb *connection(enum tcp_state state) {
    struct tcp_pcb *pcb = tcp_new();
    assert(pcb);
    IP_ADDR4(&pcb->local_ip, 192, 168, 16, 1);
    IP_ADDR4(&pcb->remote_ip, 192, 168, 16, 2);
    pcb->local_port = 80;
    pcb->remote_port = 50000;
    pcb->snd_wnd = pcb->cwnd = 4096;
    pcb->state = state;
    TCP_REG_ACTIVE(pcb);
    return pcb;
}

static struct tcp_pcb *request(uint8_t value) {
    struct tcp_pcb *pcb = connection(ESTABLISHED);
    pcb->remote_port += value;
    assert(accept(NULL, pcb, ERR_OK) == ERR_OK);
    pcb->snd_buf = 0; /* Retain the response while another request dispatches. */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, HTTP_REQUEST_MAX_BYTES, PBUF_POOL);
    uint8_t body[HTTP_REQUEST_MAX_BYTES];
    memset(body, value, sizeof(body));
    assert(p && pbuf_take(p, body, sizeof(body)) == ERR_OK);
    assert(receive(pcb->callback_arg, pcb, p, ERR_OK) == ERR_OK);
    return pcb;
}

static void check_response(struct tcp_pcb *pcb, uint8_t value) {
    connection_t *c = pcb->callback_arg;
    assert(c->responding && !c->pending);
    assert(c->response.size == HTTP_RESPONSE_MAX_BYTES);
    for (unsigned i = 0; i < HTTP_RESPONSE_MAX_BYTES; i++)
        assert(c->response.body[i] == value + 10);
}

static void resume(unsigned flags) {
    unsigned before = packets;
    network_poll();
    assert(packets == before); /* No spin or transmission while USB is busy. */
    writable = true;
    network_poll();
    assert(packets == before + 1);
    assert(packet_size >= 40 && (packet[33] & flags) == flags);
}

int main(int argc, char **argv) {
    assert(argc == 2 && network_init());
    net.output = output;
    unsigned baseline = lwip_stats.memp[MEMP_TCP_PCB]->used;
    struct tcp_pcb *pcb;
    if (!strcmp(argv[1], "empty_rx")) {
        const uint8_t bytes[] = {1, 2, 3};
        assert(!tud_network_recv_cb(bytes, 0) && !incoming);
        tud_network_recv_renew();
        assert(tud_network_recv_cb(bytes, sizeof(bytes)) && incoming);
        assert(renewals == 1);
        network_poll();
        assert(!incoming && renewals == 2);
    } else if (!strcmp(argv[1], "syn")) {
        pcb = connection(SYN_RCVD);
        assert(tcp_enqueue_flags(pcb, TCP_SYN | TCP_ACK) == ERR_OK);
        assert(tcp_output(pcb) == ERR_MEM);
        resume(TCP_SYN | TCP_ACK);
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "data")) {
        pcb = connection(ESTABLISHED);
        assert(accept(NULL, pcb, ERR_OK) == ERR_OK);
        connection_t *c = pcb->callback_arg;
        c->responding = true;
        c->response.body = (const uint8_t *)"response";
        c->response.size = 8;
        flush(c);
        resume(TCP_ACK);
        assert(packet_size == 48 && !memcmp(packet + 40, "response", 8));
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "fin")) {
        pcb = connection(ESTABLISHED);
        assert(accept(NULL, pcb, ERR_OK) == ERR_OK);
        connection_t *c = pcb->callback_arg;
        assert(sent(c, pcb, 0) == ERR_OK && !c->pcb);
        assert(pcb->state == FIN_WAIT_1 && pcb->unsent);
        resume(TCP_FIN | TCP_ACK);
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "ack")) {
        pcb = connection(ESTABLISHED);
        assert(tcp_send_empty_ack(pcb) == ERR_MEM);
        resume(TCP_ACK);
        assert(!(pcb->flags & (TF_ACK_NOW | TF_ACK_DELAY)));
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "parallel")) {
        struct tcp_pcb *clients[6];
        for (unsigned i = 0; i < 6; i++) {
            clients[i] = connection(SYN_RCVD);
            clients[i]->remote_port += i;
            assert(tcp_enqueue_flags(clients[i], TCP_SYN | TCP_ACK) == ERR_OK);
            assert(tcp_output(clients[i]) == ERR_MEM);
        }
        for (unsigned i = 0; i < 6; i++) resume(TCP_SYN | TCP_ACK);
        for (unsigned i = 0; i < 6; i++) {
            assert(!clients[i]->unsent);
            tcp_abort(clients[i]);
        }
    } else if (!strcmp(argv[1], "responses")) {
        struct tcp_pcb *clients[HTTP_CONNECTIONS];
        for (unsigned i = 0; i < HTTP_CONNECTIONS; i++) clients[i] = request(i);
        for (unsigned i = 0; i < HTTP_CONNECTIONS; i++) {
            network_poll();
            assert(dispatched == i + 1);
            for (unsigned j = 0; j <= i; j++) check_response(clients[j], j);
        }
        for (unsigned i = 0; i < HTTP_CONNECTIONS; i++) {
            clients[i]->snd_buf = TCP_SND_BUF;
            flush(clients[i]->callback_arg);
            check_response(clients[i], i);
            tcp_abort(clients[i]);
        }
    } else if (!strcmp(argv[1], "asset")) {
        pcb = request(3);
        asset_response = true;
        network_poll();
        connection_t *c = pcb->callback_arg;
        assert(c->response.body == asset && c->response.size == sizeof(asset));
        for (unsigned i = 0; i < HTTP_REQUEST_MAX_BYTES; i++) assert(c->buffer[i] == 3);
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "nested")) {
        pcb = request(1);
        struct tcp_pcb *next = request(2);
        nested_dispatch = true;
        network_poll();
        check_response(pcb, 1);
        assert(dispatched == 1 && ((connection_t *)next->callback_arg)->pending);
        network_poll();
        assert(dispatched == 2);
        check_response(pcb, 1);
        check_response(next, 2);
        tcp_abort(pcb);
        tcp_abort(next);
    } else if (!strcmp(argv[1], "response_reconnect")) {
        pcb = request(4);
        network_poll();
        check_response(pcb, 4);
        network_disconnect();
        network_poll();
        assert(!tcp_active_pcbs && ended == 1);
        pcb = request(5);
        network_poll();
        check_response(pcb, 5);
        tcp_abort(pcb);
    } else if (!strcmp(argv[1], "late_data")) {
        pcb = request(4);
        network_poll();
        check_response(pcb, 4);
        connection_t *c = pcb->callback_arg;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, 1, PBUF_POOL);
        assert(p && pbuf_take(p, "x", 1) == ERR_OK);
        assert(receive(c, pcb, p, ERR_OK) == ERR_ABRT && !c->pcb);
        assert(dispatched == 1);
    } else {
        assert(!strcmp(argv[1], "disconnect"));
        pcb = connection(SYN_RCVD);
        assert(tcp_enqueue_flags(pcb, TCP_SYN | TCP_ACK) == ERR_OK);
        assert(tcp_output(pcb) == ERR_MEM);
        pcb = connection(ESTABLISHED);
        assert(accept(NULL, pcb, ERR_OK) == ERR_OK);
        assert(sent(pcb->callback_arg, pcb, 0) == ERR_OK);
        pcb = connection(ESTABLISHED);
        assert(accept(NULL, pcb, ERR_OK) == ERR_OK);
        const uint8_t bytes[] = {1, 2, 3};
        assert(tud_network_recv_cb(bytes, sizeof(bytes)));
        ready = false;
        network_disconnect();
        network_poll();
        assert(!tcp_active_pcbs && !incoming && ended == 1 && !packets);
        ready = writable = true;
        network_poll();
        assert(!packets);
        for (unsigned i = 0; i < HTTP_CONNECTIONS; i++) assert(!connections[i].pcb);
    }
    assert(lwip_stats.memp[MEMP_TCP_PCB]->used == baseline);
    assert(lwip_stats.memp[MEMP_TCP_SEG]->used == 0);
    return 0;
}
