#ifndef INCLUDE_SERVER_H
#define INCLUDE_SERVER_H

#include "esp_netif.h"
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include "command.h"

struct server {
   struct sockaddr_in listen_address;
   ip4_addr_t *ip_address;
   struct pollfd sockets[4];
   int client_priorities[3];
   union ClientCommand client_commands[3];
   ssize_t client_cmdbuf_position[3];
};

struct server create_server(void);
int start_listening(struct server *, unsigned short);
int serve(struct server *);

#endif
