/*
 * Minimal DHCP server for the Ultra's point-to-point USB Ethernet link.
 *
 * The Pico SDK's bundled TinyUSB server assumes a contiguous input pbuf,
 * keeps its only lease assigned forever, and only accepts DHCPREQUEST packets
 * containing option 50.  Those assumptions are not safe for mobile clients,
 * which may split a packet, rotate their MAC address, or renew via ciaddr.
 */

#include "dhcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwip/def.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u
#define DHCP_FIXED_SIZE 236u
#define DHCP_COOKIE_SIZE 4u
#define DHCP_MIN_SIZE (DHCP_FIXED_SIZE + DHCP_COOKIE_SIZE)
#define DHCP_MIN_REPLY_SIZE 300u
#define DHCP_LEASE_SECONDS (24u * 60u * 60u)

enum {
    DHCP_BOOTREQUEST = 1u,
    DHCP_BOOTREPLY = 2u,
    DHCP_DISCOVER = 1u,
    DHCP_OFFER = 2u,
    DHCP_REQUEST = 3u,
    DHCP_ACK = 5u,
    DHCP_NAK = 6u,
};

enum {
    DHCP_OPTION_PAD = 0u,
    DHCP_OPTION_SUBNET_MASK = 1u,
    DHCP_OPTION_REQUESTED_IP = 50u,
    DHCP_OPTION_LEASE_TIME = 51u,
    DHCP_OPTION_MESSAGE_TYPE = 53u,
    DHCP_OPTION_SERVER_ID = 54u,
    DHCP_OPTION_END = 255u,
};

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t legacy[192];
    uint8_t cookie[4];
    uint8_t options[312];
} dhcp_message_t;

typedef struct {
    struct udp_pcb *pcb;
    struct netif *netif;
    ip4_addr_t server;
    ip4_addr_t netmask;
    ip4_addr_t lease;
} dhcp_server_t;

static dhcp_server_t dhcp_server;
static dhcp_message_t dhcp_message;
static const uint8_t dhcp_cookie[DHCP_COOKIE_SIZE] = {99u, 130u, 83u, 99u};

static void write_ip(uint8_t destination[4], const ip4_addr_t *address) {
    memcpy(destination, &address->addr, sizeof(address->addr));
}

static ip4_addr_t read_ip(const uint8_t source[4]) {
    ip4_addr_t address;
    memcpy(&address.addr, source, sizeof(address.addr));
    return address;
}

static const uint8_t *find_option(const uint8_t *options, size_t options_size, uint8_t wanted) {
    size_t offset = 0u;
    while (offset < options_size) {
        const uint8_t option = options[offset];
        if (option == DHCP_OPTION_END) {
            return NULL;
        }
        if (option == DHCP_OPTION_PAD) {
            ++offset;
            continue;
        }
        if (offset + 1u >= options_size) {
            return NULL;
        }
        const size_t length = options[offset + 1u];
        if (offset + 2u + length > options_size) {
            return NULL;
        }
        if (option == wanted) {
            return &options[offset];
        }
        offset += 2u + length;
    }
    return NULL;
}

static void append_u8(uint8_t **cursor, uint8_t option, uint8_t value) {
    *(*cursor)++ = option;
    *(*cursor)++ = 1u;
    *(*cursor)++ = value;
}

static void append_u32(uint8_t **cursor, uint8_t option, uint32_t value) {
    *(*cursor)++ = option;
    *(*cursor)++ = 4u;
    *(*cursor)++ = (uint8_t)(value >> 24u);
    *(*cursor)++ = (uint8_t)(value >> 16u);
    *(*cursor)++ = (uint8_t)(value >> 8u);
    *(*cursor)++ = (uint8_t)value;
}

static void append_ip(uint8_t **cursor, uint8_t option, const ip4_addr_t *address) {
    *(*cursor)++ = option;
    *(*cursor)++ = 4u;
    memcpy(*cursor, &address->addr, sizeof(address->addr));
    *cursor += sizeof(address->addr);
}

static bool option_ip(const uint8_t *option, ip4_addr_t *address) {
    if (option == NULL || option[1] != 4u) {
        return false;
    }
    memcpy(&address->addr, &option[2], sizeof(address->addr));
    return true;
}

static bool request_selects_this_server(const dhcp_message_t *message, size_t options_size) {
    ip4_addr_t requested;
    const uint8_t *server_id = find_option(message->options, options_size, DHCP_OPTION_SERVER_ID);
    if (option_ip(server_id, &requested) && !ip4_addr_cmp(&requested, &dhcp_server.server)) {
        return false;
    }
    return true;
}

static bool requested_address(const dhcp_message_t *message, size_t options_size,
                              ip4_addr_t *requested) {
    const uint8_t *requested_option =
        find_option(message->options, options_size, DHCP_OPTION_REQUESTED_IP);
    if (option_ip(requested_option, requested)) {
        return true;
    }

    *requested = read_ip(message->ciaddr);
    return !ip4_addr_isany_val(*requested);
}

static bool address_is_leaseable(const ip4_addr_t *address) {
    const uint32_t mask = ip4_addr_get_u32(&dhcp_server.netmask);
    const uint32_t server = ip4_addr_get_u32(&dhcp_server.server);
    const uint32_t candidate = ip4_addr_get_u32(address);
    const uint32_t network = server & mask;
    const uint32_t broadcast = network | ~mask;
    return (candidate & mask) == network && candidate != network && candidate != broadcast &&
           candidate != server;
}

static void send_reply(struct udp_pcb *pcb, uint8_t message_type, const ip4_addr_t *address) {
    dhcp_message.op = DHCP_BOOTREPLY;
    dhcp_message.hops = 0u;
    dhcp_message.secs = 0u;
    memset(dhcp_message.ciaddr, 0, sizeof(dhcp_message.ciaddr));
    if (address == NULL) {
        memset(dhcp_message.yiaddr, 0, sizeof(dhcp_message.yiaddr));
    } else {
        write_ip(dhcp_message.yiaddr, address);
    }
    memset(dhcp_message.siaddr, 0, sizeof(dhcp_message.siaddr));
    memset(dhcp_message.giaddr, 0, sizeof(dhcp_message.giaddr));
    memcpy(dhcp_message.cookie, dhcp_cookie, sizeof(dhcp_cookie));
    memset(dhcp_message.options, 0, sizeof(dhcp_message.options));

    uint8_t *cursor = dhcp_message.options;
    append_u8(&cursor, DHCP_OPTION_MESSAGE_TYPE, message_type);
    append_ip(&cursor, DHCP_OPTION_SERVER_ID, &dhcp_server.server);
    if (message_type != DHCP_NAK) {
        append_u32(&cursor, DHCP_OPTION_LEASE_TIME, DHCP_LEASE_SECONDS);
        append_ip(&cursor, DHCP_OPTION_SUBNET_MASK, &dhcp_server.netmask);
    }
    *cursor++ = DHCP_OPTION_END;

    const size_t used_reply_size = DHCP_MIN_SIZE + (size_t)(cursor - dhcp_message.options);
    /* BOOTP requires at least 300 octets in the UDP payload.  Some desktop
     * clients accept shorter DHCP replies, but conforming clients may drop
     * them silently.  The options buffer is already zero-filled, so bytes
     * following END are valid PAD options. */
    const size_t reply_size =
        used_reply_size < DHCP_MIN_REPLY_SIZE ? DHCP_MIN_REPLY_SIZE : used_reply_size;
    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, (u16_t)reply_size, PBUF_RAM);
    if (reply == NULL || pbuf_take(reply, &dhcp_message, (u16_t)reply_size) != ERR_OK) {
        if (reply != NULL) {
            pbuf_free(reply);
        }
        return;
    }

    /* Broadcast replies so clients can receive them before configuring IPv4. */
    const ip_addr_t destination = IPADDR4_INIT_BYTES(255, 255, 255, 255);
    (void)udp_sendto_if(pcb, reply, &destination, DHCP_CLIENT_PORT, dhcp_server.netif);
    pbuf_free(reply);
}

static void receive(void *argument, struct udp_pcb *pcb, struct pbuf *packet,
                    const ip_addr_t *source, u16_t port) {
    (void)argument;
    (void)source;
    (void)port;

    if (packet->tot_len < DHCP_MIN_SIZE) {
        pbuf_free(packet);
        return;
    }

    const size_t copied = pbuf_copy_partial(packet, &dhcp_message, sizeof(dhcp_message), 0u);
    pbuf_free(packet);
    if (copied < DHCP_MIN_SIZE || dhcp_message.op != DHCP_BOOTREQUEST || dhcp_message.htype != 1u ||
        dhcp_message.hlen != 6u ||
        memcmp(dhcp_message.cookie, dhcp_cookie, sizeof(dhcp_cookie)) != 0) {
        return;
    }

    const size_t options_size = copied - DHCP_MIN_SIZE;
    const uint8_t *type = find_option(dhcp_message.options, options_size, DHCP_OPTION_MESSAGE_TYPE);
    if (type == NULL || type[1] != 1u) {
        return;
    }

    ip4_addr_t requested;
    switch (type[2]) {
    case DHCP_DISCOVER:
        /* Only one USB host exists.  A new MAC replaces a stale desktop
         * or iOS private-address lease immediately. */

        if (requested_address(&dhcp_message, options_size, &requested) &&
            address_is_leaseable(&requested)) {
            ip4_addr_copy(dhcp_server.lease, requested);
        }

        send_reply(pcb, DHCP_OFFER, &dhcp_server.lease);
        break;
    case DHCP_REQUEST:

        if (!request_selects_this_server(&dhcp_message, options_size)) {
            break;
        }
        if (!requested_address(&dhcp_message, options_size, &requested)) {
            ip4_addr_copy(requested, dhcp_server.lease);
        }

        if (address_is_leaseable(&requested)) {
            ip4_addr_copy(dhcp_server.lease, requested);
            send_reply(pcb, DHCP_ACK, &dhcp_server.lease);
        } else {
            send_reply(pcb, DHCP_NAK, NULL);
        }
        break;
    default:
        break;
    }
}

err_t dhcp_server_init(struct netif *netif, const ip4_addr_t *server, const ip4_addr_t *netmask,
                    const ip4_addr_t *lease) {
    if (netif == NULL || server == NULL || netmask == NULL || lease == NULL) {
        return ERR_ARG;
    }

    if (dhcp_server.pcb != NULL) {
        udp_remove(dhcp_server.pcb);
    }
    memset(&dhcp_server, 0, sizeof(dhcp_server));
    dhcp_server.netif = netif;
    ip4_addr_copy(dhcp_server.server, *server);
    ip4_addr_copy(dhcp_server.netmask, *netmask);
    ip4_addr_copy(dhcp_server.lease, *lease);

    dhcp_server.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (dhcp_server.pcb == NULL) {
        return ERR_MEM;
    }
    const err_t result = udp_bind(dhcp_server.pcb, IP_ANY_TYPE, DHCP_SERVER_PORT);
    if (result != ERR_OK) {
        udp_remove(dhcp_server.pcb);
        dhcp_server.pcb = NULL;
        return result;
    }
    udp_recv(dhcp_server.pcb, receive, NULL);
    return ERR_OK;
}
