#ifndef INCLUDE_UTIL_H
#define INCLUDE_UTIL_H

inline void delay(uint64_t millis) {
    vTaskDelay(millis / portTICK_PERIOD_MS);
}

inline uint64_t millis(void) {
    return esp_timer_get_time() / 1000;
}

#endif
