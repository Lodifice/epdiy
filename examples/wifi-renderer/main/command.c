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

#define LINE_SIZE (EPD_WIDTH / 16)
#define LINE_QUEUE_LEN 384

static StaticQueue_t xQueueBuffer;
static uint8_t* queue_storage;
static QueueHandle_t line_queue;

SemaphoreHandle_t start_semaphore;
SemaphoreHandle_t end_semaphore;

typedef struct {
    uint32_t line_buf[LINE_SIZE];
    uint32_t line_pos;
    uint32_t out_of_run_counter;
} DecompressionState;

static DecompressionState decomp_state;

static inline int consume_remaining_out_of_run(const uint32_t* input, size_t consumable_words) {

    const uint32_t* input_start = input;

    while (decomp_state.out_of_run_counter > 0 && input - input_start < consumable_words) {
        decomp_state.line_buf[decomp_state.line_pos] = *(input++);
        decomp_state.line_pos++;
        // line done
        if (decomp_state.line_pos == LINE_SIZE) {
            xQueueSendToBack(line_queue, (void*)(&decomp_state.line_buf[0]), portMAX_DELAY);
            decomp_state.line_pos = 0;
        }
        decomp_state.out_of_run_counter--;
    }
    return input - input_start;
}

int IRAM_ATTR decompress_line(const uint32_t* input, size_t input_size) {
    const uint32_t* input_start = input;

    // consumable input in words
    size_t consumable_input = input_size / 4;

    //ESP_LOGI("comp", "consumable input: %d of %d, decomp ctr: %d", consumable_input, input_size, decomp_state.out_of_run_counter);
    if (decomp_state.out_of_run_counter > 0) {
        input += consume_remaining_out_of_run(input, consumable_input);
    }
    while (input - input_start + 1 < consumable_input) {

        // get the next marker
        uint32_t marker = *(input++);
        assert((marker & 0xF0000000) == 0xF0000000);
        uint32_t run_counter = (marker & 0xFFFF);
        decomp_state.out_of_run_counter = (marker & 0x0FFF0000) >> 16;
        uint32_t run_value = *(input++);
        //ESP_LOGI("comp", "%d run of %X, %d run of uncompressed input.", run_counter, run_value, decomp_state.out_of_run_counter);

        while (run_counter > 0) {
            decomp_state.line_buf[decomp_state.line_pos] = run_value;
            decomp_state.line_pos++;
            // line done
            if (decomp_state.line_pos == LINE_SIZE) {
                xQueueSendToBack(line_queue, (void*)(&decomp_state.line_buf[0]), portMAX_DELAY);
                decomp_state.line_pos = 0;
            }
            run_counter--;
        }

        size_t remaining_consumable = consumable_input - (input - input_start);
        input += consume_remaining_out_of_run(input, remaining_consumable);

    }
    return (uint8_t*)input - (uint8_t*)input_start;
}

void IRAM_ATTR draw_lines(const DrawCmd* cmd, int fd) {

    ssize_t total_consumed = 0;
    ssize_t buffer_len = 0;
    uint8_t* buf = decode_buf;

    xSemaphoreGive(start_semaphore);

    while (total_consumed < cmd->size) {
        ssize_t left = cmd->size - total_consumed - buffer_len;
        ssize_t to_read = (left < DECODE_BUF_SIZE - buffer_len) ? left : DECODE_BUF_SIZE - buffer_len;
        ssize_t len_read = recv(fd, buf + buffer_len, to_read, 0);
        buffer_len += len_read;

        int shrink_by = decompress_line((uint32_t*)buf, buffer_len);
        //ESP_LOGI("comp", "shrink by: %d of %d", shrink_by, buffer_len);
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
            epd_output_row(120);
        }
        epd_end_frame();
        xSemaphoreGive(end_semaphore);
    }
}


void initialize_display_data() {
    epd_base_init(EPD_WIDTH);
    queue_storage = heap_caps_malloc(LINE_QUEUE_LEN * EPD_WIDTH / 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    assert(queue_storage != NULL);
    line_queue = xQueueCreateStatic(LINE_QUEUE_LEN, EPD_WIDTH / 4, queue_storage, &xQueueBuffer);
    decode_buf = malloc(DECODE_BUF_SIZE);
    assert(decode_buf != NULL);
    start_semaphore = xSemaphoreCreateBinary();
    end_semaphore = xSemaphoreCreateBinary();
    RTOS_ERROR_CHECK(xTaskCreatePinnedToCore(&feed_display, "feed_display", 1 << 12, NULL, 1, NULL, 1));
}
