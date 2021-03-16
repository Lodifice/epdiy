#pragma once
#include "esp_types.h"

typedef struct __attribute__((__packed__)) {
    char tag;
    uint32_t offset;
    uint32_t amount;
    uint32_t time;
    uint32_t size;
} DrawCmd;

typedef struct __attribute__((__packed__)) {
    char tag;
    uint32_t status;
    uint32_t _reserved1;
    uint32_t _reserved2;
    uint32_t _reserved3;
} PowerCmd;

typedef struct __attribute__((__packed__)) {
    char tag;
    uint32_t priority;
    uint32_t _reserved1;
    uint32_t _reserved2;
    uint32_t _reserved3;
} HelloCmd;

typedef struct __attribute__((__packed__)) {
    char tag;
    uint32_t _reserved1;
    uint32_t _reserved2;
    uint32_t _reserved3;
    uint32_t _reserved4;
} GoodbyeCmd;

union ClientCommand {
    char tag;
    DrawCmd draw;
    HelloCmd hello;
    PowerCmd power;
    GoodbyeCmd goodbye;
};

_Static_assert(sizeof(DrawCmd) == sizeof(union ClientCommand), "inconsistent command size");
_Static_assert(sizeof(HelloCmd) == sizeof(union ClientCommand), "inconsistent command size");

void IRAM_ATTR draw_lines(const DrawCmd* cmd, int fd);

void initialize_display_data();
