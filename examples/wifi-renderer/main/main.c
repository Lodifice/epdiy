/* Simple firmware for a ESP32 displaying a static image on an EPaper Screen.
 *
 * Write an image into a header file using a 3...2...1...0 format per pixel,
 * for 4 bits color (16 colors - well, greys.) MSB first.  At 80 MHz, screen
 * clears execute in 1.075 seconds and images are drawn in 1.531 seconds.
 */

#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_types.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "epd_driver.h"

#include "server.h"
#include "command.h"
#include "util.h"
#include "wifi.h"

#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)

#define BUF_SIZE (4096)
#define ESC_BUF_SIZE (128 * 4)
#define ESC_ARG_SIZE  16

#define PORT 1337

static const char *TAG = "wifi-renderer";

void serve_task() {

    initialize_display_data();
    struct server server = create_server();
    server.ip_address = connect_to_wifi();
    ESP_LOGI(TAG, "network connection established");
    ESP_LOGI(TAG, "ipv4 address: " IPSTR, IP2STR(server.ip_address));

    int ret = start_listening(&server, PORT);
    // TODO handle errors
    ESP_LOGI(TAG, "listening\n");

    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);


    while (true) {
        serve(&server);
    }
}

void app_main() {
    /*
    epd_poweron();
    epd_clear();
    epd_poweroff();
    */

    xTaskCreate(&serve_task, "serve_task", 1 << 12, NULL, 1, NULL);
}
