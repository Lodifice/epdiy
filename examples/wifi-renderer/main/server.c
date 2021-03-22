#include "server.h"

#include "command.h"
#include "esp_err.h"
#include "esp_types.h"
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include "epd_driver.h"

#include "protocol.h"

#define listen_socket(srv) (srv).sockets[0].fd
#define nsockets(srv) sizeof (srv).sockets / sizeof (srv).sockets[0]

#define POLL_TIMEOUT 2000

static const char *TAG = "wifi-renderer";
int64_t t1, t2;

static void notify_client(struct server *, int, enum opcode);

static void print_debug(struct server *server) {
    ESP_LOGI(TAG, "Listen fd %d | events %d | revents %d",
            server->sockets[0].fd, server->sockets[0].events,
            server->sockets[0].revents);
    for (int i = 1; i < nsockets(*server); ++i) {
        ESP_LOGI(TAG, "Cl %d | fd %d | events %d | revents %d | prio %d",
                 i, server->sockets[i].fd, server->sockets[i].events,
                 server->sockets[i].revents, server->client_priorities[i-1]);
    }
    ESP_LOGI(TAG, "Active cl %d", server->active_client);
}

struct server create_server(void) {
    struct server server;
    memset(&server, 0, sizeof server);
    for (int i = 0; i < sizeof server.sockets / sizeof server.sockets[0]; ++i) {
        server.sockets[i] = (struct pollfd){ .fd = -1, .events = POLLIN, .revents = 0 };
    }
    for (int i = 1; i < nsockets(server); ++i) {
        server.client_priorities[i-1] = -1;
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
    int err = setsockopt(listen_socket(*server), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to reuse address: errno %d", errno);
        goto ABORT;
    }
    err = ioctl(listen_socket(*server), FIONBIO, (char *)&opt);
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to set to non-blocking: errno %d", errno);
        goto ABORT;
    }
    ESP_LOGI(TAG, "socket created and set up");

    err = bind(listen_socket(*server),
            (struct sockaddr *)&server->listen_address,
            sizeof server->listen_address);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", AF_INET);
        goto ABORT;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", port);

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

static bool try_add_client(struct server *server, int client_socket) {
    for (int i = 1; i < nsockets(*server); ++i) {
        if (server->sockets[i].fd == -1) {
            server->sockets[i].fd = client_socket;
            server->client_cmdbuf_position[i-1] = 0;
            ESP_LOGI(TAG, "Accepted client %d", i);
            return true;
        }
    }
    return false;
}

static void remove_client(struct server *server, int client, bool notify) {
    server->client_priorities[client-1] = -1;
    if (notify) {
        notify_client(server, client, SERVER_GOODBYE);
    }
    close(server->sockets[client].fd);
    server->sockets[client].fd = -1;
    ESP_LOGI(TAG, "Removed client %d", client);
    if (server->active_client != client || server->active_client == 0) {
        return;
    }
    for (int i = 1; i < nsockets(*server); ++i) {
        if (server->client_priorities[i-1] > server->client_priorities[server->active_client-1]) {
            server->active_client = i;
        }
    }
    if (server->active_client != client) {
        ESP_LOGI(TAG, "New active client %d", server->active_client);
        notify_client(server, server->active_client, SERVER_ACTIVATED_CLIENT);
    } else {
        ESP_LOGI(TAG, "No active client remains");
        server->active_client = 0;
    }
}

static void prioritize_client(struct server *server, int client, int priority) {
    server->client_priorities[client-1] = priority;
    ESP_LOGI(TAG, "prioritized client %d with %d.", client, priority);
    print_debug(server);
    if (priority > server->client_priorities[server->active_client-1]) {
        if (server->active_client > 0) {
            notify_client(server, server->active_client, SERVER_ENQUEUED_CLIENT);
        }
        server->active_client = client;
        notify_client(server, client, SERVER_ACTIVATED_CLIENT);
    } else {
        notify_client(server, client, SERVER_ENQUEUED_CLIENT);
    }
}

static void notify_client(struct server *server, int client, enum opcode code) {
    char answer = code;
    while (true) {
        ssize_t len = send(server->sockets[client].fd, &answer, sizeof answer, 0);
        if (len < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                vTaskDelay(1);
                continue;
                //ESP_LOGE(TAG, "Sending to client would block, deleting client");
            } else {
                ESP_LOGE(TAG, "Error in sending to client, deleting client");
            }
            remove_client(server, client, false);
            break;
        }
        break;
    }
}

static void handle_message(struct server *server, int client) {
    union ClientCommand cmd = server->client_commands[client-1];
    switch (cmd.tag) {
        case CLIENT_HELLO:
            prioritize_client(server, client, cmd.hello.priority);
            break;
        case CLIENT_GOODBYE:
            remove_client(server, client, true);
            break;
        case CLIENT_DRAW:
            {
                //ESP_LOGI(TAG, "Draw command with offset %d, amount %d, time %d size: %d", cmd.draw.offset, cmd.draw.amount, cmd.draw.time, cmd.draw.size);
                int64_t t1 = esp_timer_get_time();
                draw_lines(&cmd.draw, server->sockets[client].fd);
                int64_t t2 = esp_timer_get_time();
                printf("Frame received in %lld ms\n", (t2 - t1) / 1000);
                notify_client(server, client, SERVER_DRAW_SUCCESSFUL);
                break;
            }
        case CLIENT_POWER:
            ESP_LOGI(TAG, "Power status: %d", cmd.power.status);
            if (cmd.power.status) {
                epd_poweron();
            } else {
                epd_poweroff();
            }
            break;
        default: {
            notify_client(server, client, SERVER_INVALID);
            ESP_LOGI(TAG, "unknown command: %c", cmd.tag);
        }
    }
}

int serve(struct server *server) {
    int ret = 0;
    do {
        ret = poll(server->sockets, nsockets(*server), POLL_TIMEOUT);
        if (ret <= 0) {
            continue;
        }
        //print_debug(server);
        //ESP_LOGI(TAG, "Poll: %d", ret);
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
                int fd = -1;
                do {
                    fd = accept(listen_socket(*server), NULL, NULL);
                    if (fd < 0) {
                        if (errno == EWOULDBLOCK || errno == EAGAIN) {
                            break;
                        }
                        ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                        return fd;
                    }
                    // This should be inherited from the listening socket
                    // ... but apparently not on our platform
                    int opt = 1;
                    int err = ioctl(fd, FIONBIO, (char *)&opt);
                    if (err < 0) {
                        ESP_LOGE(TAG, "Socket unable to set to non-blocking: errno %d", errno);
                    }
                    if (!try_add_client(server, fd)) {
                        close(fd);
                    }
                } while (fd != - 1);
                continue;
            }

            ssize_t len_read = -1;
            do {
                // socket of client connection
                ssize_t bufpos = server->client_cmdbuf_position[i - 1];
                ssize_t maximum_read = sizeof(union ClientCommand) - bufpos;
                uint8_t *cmdbuf = ((uint8_t*)&server->client_commands[i - 1]) + bufpos;
                len_read = recv(server->sockets[i].fd, cmdbuf, maximum_read, 0);
                if (len_read < 0) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        break;
                    }
                    ESP_LOGE(TAG, "Error reading from socket: errno %d", errno);
                    remove_client(server, i, false);
                    break;
                }
                if (len_read == 0) {
                    remove_client(server, i, false);
                    break;
                }
                bufpos += len_read;
                // command is complete
                if (bufpos == sizeof(union ClientCommand)) {
                    handle_message(server, i);
                    bufpos = 0;
                }
                server->client_cmdbuf_position[i - 1] = bufpos;
                // FIXME removing client in handle message leads to EBADF on
                // the next recv
                // doesn't matter, but should be done nicer
            } while (len_read > 0);
        }
    } while (ret >= 0);
    return ret;
}
