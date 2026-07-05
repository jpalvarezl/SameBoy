#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "defs.h"

typedef struct {
    uint8_t byte_being_received;
    uint8_t bits_received;
    uint8_t byte_to_send;
    bool bit_to_send;
    // a tiny ring buffer of MIDI bytes queued from the host:
    uint8_t queue[256];
    uint8_t queue_head, queue_tail;
} GB_midi_t;

void GB_connect_midi(GB_gameboy_t *gb);
bool GB_midi_is_enabled(GB_gameboy_t *gb);
void GB_midi_input_byte(GB_gameboy_t *gb, uint8_t byte);