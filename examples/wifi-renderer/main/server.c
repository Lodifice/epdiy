#include "server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define listen_socket(srv) (srv).sockets[0].fd
#define nsockets(srv) sizeof (srv).sockets / sizeof (srv).sockets[0]

#define POLL_TIMEOUT 1000

static const char *TAG = "wifi-renderer";

struct server create_server(void) {
    struct server server;
    memset(&server, 0, sizeof server);
    for (int i = 0; i < sizeof server.sockets / sizeof server.sockets[0]; ++i) {
        server.sockets[i] = (struct pollfd){ .fd = -1, .events = POLLIN, .revents = 0 };
    }
    return server;
}

int start_listening(struct server *server, unsigned short port) {
    server->listen_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server->listen_address.sin_family = AF_INET;
    server->listen_address.sin_port = htons(port);

    listen_socket(*server) = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (listen_socket(*server) < 0) {
        ESP_LOGE(TAG, "unable to create socket: errno %d", errno);
        return -1;
    }
    int opt = 1;
    setsockopt(listen_socket(*server), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ESP_LOGD(TAG, "socket created");

    int err = bind(listen_socket(*server),
            (struct sockaddr *)&server->listen_address,
            sizeof server->listen_address);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", AF_INET);
        goto ABORT;
    }
    ESP_LOGD(TAG, "Socket bound, port %d", port);

    err = listen(listen_socket(*server), 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto ABORT;
    }

    return err;

ABORT:
    close(listen_socket(*server));
    listen_socket(*server) = -1;
    return err;
}

int serve(struct server *server) {
    int ret = 0;
    do {
        ret = poll(server->sockets, nsockets(*server), POLL_TIMEOUT);
        if (ret <= 0) {
            continue;
        }
        for (int i = 0; i < nsockets(*server); ++i) {
            if (server->sockets[i].revents == 0) {
                continue;
            }
            if (server->sockets[i].revents != POLLIN) {
                // TODO log unexpected event
                // maybe just ignore it?
                ret = 0;
                break;
            }
            // listening socket
            if (i == 0) {
                int fd = accept(listen_socket(*server), NULL, NULL);
                if (fd < 0) {
                    ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                    return fd;
                }
                bool enqueued = false;
                for (int j = 1; j < nsockets(*server); ++j) {
                    if (server->sockets[j].fd == -1) {
                        server->sockets[j].fd = fd;
                        enqueued = true;
                        break;
                    }
                }
                if (!enqueued) {
                    close(fd);
                }
                continue;
            }
            // socket of client connection
            char buffer[256];
            ssize_t len_read = recv(server->sockets[i].fd, buffer, sizeof buffer, 0);
            if (len_read < 0) {
                ESP_LOGE(TAG, "Error reading from socket: errno %d", errno);
                return -1;
            }
            if (len_read == 0) {
                // TODO connection closed, clean stuff up
                close(server->sockets[i].fd);
                continue;
            }
            ESP_LOGI(TAG, "Message: %s", buffer);
        }
    } while (ret >= 0);
    return ret;
}
