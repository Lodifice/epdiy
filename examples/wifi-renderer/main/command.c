#include "epd_driver.h"
#include "command.h"
#include "esp_attr.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"


#define RTOS_ERROR_CHECK(x)                                                    \
  do {                                                                         \
    esp_err_t __err_rc = (x);                                                  \
    if (__err_rc != pdPASS) {                                                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

#define MIN(x,y) (x < y ? x : y)

static uint8_t* decode_buf;
const int DECODE_BUF_SIZE = 1 << 12;

extern void epd_base_init(uint32_t epd_row_width);
extern void epd_start_frame();
extern void epd_end_frame();
extern void IRAM_ATTR epd_output_row(uint32_t output_time_dus);
extern void IRAM_ATTR epd_switch_buffer();
extern uint8_t IRAM_ATTR *epd_get_current_buffer();

#define LINE_SIZE (EPD_WIDTH / 4)
#define LINE_QUEUE_LEN 384

static StaticQueue_t xQueueBuffer;
static uint8_t* queue_storage;
static QueueHandle_t line_queue;

SemaphoreHandle_t start_semaphore;
SemaphoreHandle_t end_semaphore;

int IRAM_ATTR decompress_line(const uint8_t* input, size_t input_size) {
    static uint8_t line_buf[LINE_SIZE];
    static int line_pos = 0;
    static int total_bytes = 0;

    int consumed = 0;
    int sequence_length = 0;
    uint8_t sequence_value = 0;
    while (consumed < input_size || sequence_length > 0) {

        uint8_t value = 0;
        // not in a compressed sequence, continue with input
        if (sequence_length == 0) {
            value = *input;
            input++;
            consumed++;
        } else {
            int remaining_in_line = LINE_SIZE - line_pos;
            int to_set = MIN(remaining_in_line, sequence_length);
            memset(&line_buf[line_pos], sequence_value, to_set);
            line_pos += to_set;
            sequence_length -= to_set;
            if (line_pos == LINE_SIZE) {
                xQueueSendToBack(line_queue, (void*)(&line_buf[0]), portMAX_DELAY);
                line_pos = 0;
            }
            continue;
        }

        // non-compressed pixel
        if (value != 0xFF) {
            line_buf[line_pos] = value;
            line_pos++;
            total_bytes++;
            // line done
            if (line_pos == LINE_SIZE) {
                xQueueSendToBack(line_queue, (void*)(&line_buf[0]), portMAX_DELAY);
                line_pos = 0;
            }
        // a compressed run of data
        } else {
            // rest of the compression head is available in input
            if (consumed + 2 <= input_size) {
                sequence_length = *(input++) + 2;
                sequence_value = *(input++);
                consumed += 2;
            // compression head is incomplete, abort here.
            } else {
                consumed--;
                break;
            }
        }
    }
    assert(sequence_length == 0);
    return consumed;
}

void IRAM_ATTR draw_lines(const DrawCmd* cmd, int fd) {

    ssize_t total_consumed = 0;
    ssize_t buffer_len = 0;
    ssize_t data_position = 0;
    uint8_t* buf = decode_buf;

    xSemaphoreGive(start_semaphore);

    while (total_consumed < cmd->size) {
        ssize_t left = cmd->size - total_consumed - buffer_len;
        ssize_t to_read = (left < DECODE_BUF_SIZE - buffer_len) ? left : DECODE_BUF_SIZE - buffer_len;
        ssize_t len_read = recv(fd, buf + buffer_len, to_read, 0);
        buffer_len += len_read;

        int shrink_by = decompress_line(buf, buffer_len);
        total_consumed += shrink_by;
        if (shrink_by > 0) {
            int remaining = buffer_len - shrink_by;
            memmove(buf, buf + buffer_len - remaining, remaining);
            buffer_len -= shrink_by;
        }
    }

    xSemaphoreTake(end_semaphore, portMAX_DELAY);
}

void IRAM_ATTR feed_display() {
    while (true) {
        xSemaphoreTake(start_semaphore, portMAX_DELAY);
        epd_start_frame();
        for (int h = 0; h < EPD_HEIGHT; h++) {
            xQueueReceive(line_queue, epd_get_current_buffer(), portMAX_DELAY);
            epd_output_row(20);
            epd_switch_buffer();
        }
        epd_end_frame();
        xSemaphoreGive(end_semaphore);
    }
}


void initialize_display_data() {
    epd_base_init(EPD_WIDTH);
    queue_storage = heap_caps_malloc(LINE_QUEUE_LEN * LINE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    assert(queue_storage != NULL);
    line_queue = xQueueCreateStatic(LINE_QUEUE_LEN, LINE_SIZE, queue_storage, &xQueueBuffer);
    decode_buf = malloc(DECODE_BUF_SIZE);
    assert(decode_buf != NULL);
    start_semaphore = xSemaphoreCreateBinary();
    end_semaphore = xSemaphoreCreateBinary();
    RTOS_ERROR_CHECK(xTaskCreate(&feed_display, "feed_display", 1 << 12, NULL, 1, NULL));
}
