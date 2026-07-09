#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "defs.h"

typedef void (*GB_midi_output_byte_callback_t)(GB_gameboy_t *gb, uint8_t byte);

typedef struct {
    // --------- MIDI in (host → GB): queued bytes waiting to be clocked into the GB ---------
    // a tiny ring buffer of MIDI bytes queued from the host:
    uint8_t queue[256];
    // consumer read cursor: index of the next byte to READ. GB_midi_run advances it.
    uint8_t queue_head;
    // producer write cursor: index of the next slot to WRITE. GB_midi_input_byte advances it.
    uint8_t queue_tail;

    // --------- MIDI out (GB → host): bits the GB clocks out to us ---------
    // the byte the GB is currently clocking out to us
    uint8_t byte_being_received;
    // how many of its 8 bits we've absorbed
    uint8_t bits_received;
    // (optional) bits we hand back to the GB
    uint8_t byte_to_send;
    bool bit_to_send;

    // --------- MIDI in (host → GB): the byte currently being clocked into the GB ---------
    // the byte we're currently clocking into the GB
    uint8_t byte_being_sent;
    // how many of its bits remain (8 → 0)
    uint8_t bits_left;
} GB_midi_t;

void GB_connect_midi(GB_gameboy_t *gb, GB_midi_output_byte_callback_t cb);
bool GB_midi_is_enabled(GB_gameboy_t *gb);
void GB_midi_input_byte(GB_gameboy_t *gb, uint8_t byte);
#ifdef GB_INTERNAL
internal void GB_midi_run(GB_gameboy_t *gb);
#endif