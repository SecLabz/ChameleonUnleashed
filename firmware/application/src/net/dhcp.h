#pragma once

#include "lwip/err.h"
#include "lwip/ip4_addr.h"

struct netif;

err_t dhcp_server_init(struct netif *netif, const ip4_addr_t *server, const ip4_addr_t *netmask,
                    const ip4_addr_t *lease);
