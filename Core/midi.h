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
    // consumer read cursor: index of the next byte to READ. GB_midi_run advances it.
    _Atomic uint8_t queue_head;
    // producer write cursor: index of the next slot to WRITE. GB_midi_input_byte advances it.
    _Atomic uint8_t queue_tail;

    // the byte we're currently clocking into the GB
    uint8_t byte_being_sent;
    // how many of its bits remain (8 → 0)
    uint8_t bits_left;
} GB_midi_t;

void GB_connect_midi(GB_gameboy_t *gb);
bool GB_midi_is_enabled(GB_gameboy_t *gb);
void GB_midi_input_byte(GB_gameboy_t *gb, uint8_t byte);
#ifdef GB_INTERNAL
internal void GB_midi_run(GB_gameboy_t *gb);
#endif