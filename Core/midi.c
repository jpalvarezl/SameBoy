#include "gb.h"
#include <string.h>

static void serial_start(GB_gameboy_t *gb, bool bit_received) {
    gb->midi.byte_being_received <<= 1;
    gb->midi.byte_being_received |= bit_received ? 1 : 0;
    gb->midi.bits_received++;

    if (gb->midi.bits_received == 8) {
        GB_log(gb, "MIDI out: %02X\n", gb->midi.byte_being_received);
        gb->midi.bits_received = 0;
        gb->midi.byte_being_received = 0;
    }
}

static bool serial_end(GB_gameboy_t *gb) { 
    // TODO: protocol hook
    return true; 
}

void GB_connect_midi(GB_gameboy_t *gb) {
    GB_ASSERT_NOT_RUNNING_OTHER_THREAD(gb)
    memset(&gb->midi, 0, sizeof(gb->midi));
    GB_set_serial_transfer_bit_start_callback(gb, serial_start);
    GB_set_serial_transfer_bit_end_callback(gb, serial_end);
    gb->accessory = GB_ACCESSORY_MIDI;
}

bool GB_midi_is_enabled(GB_gameboy_t *gb) {
    return gb->accessory == GB_ACCESSORY_MIDI;
}

void GB_midi_input_byte(GB_gameboy_t *gb, uint8_t byte) {
    if (gb->accessory != GB_ACCESSORY_MIDI) {
        GB_log(gb, "GB_midi_input_byte called when MIDI is not enabled.\n");
        return;
    }
    uint8_t next = gb->midi.queue_tail + 1;
    if (next == gb->midi.queue_head) {
        /* Queue full: drop the byte silently. This only happens if the Game Boy has stopped
           draining the serial port, and per-byte logging here could flood a realtime path. */
        return;
    }
    gb->midi.queue[gb->midi.queue_tail] = byte;
    gb->midi.queue_tail = next;
}

void GB_midi_run(GB_gameboy_t *gb) {
    if (gb->accessory != GB_ACCESSORY_MIDI) {
        return;
    }
    // Looking at bit7 and bit0. We are looking for the serial port to be armed and expecting external clock
    // Therefore we are looking for 0b10000001 to be 0b10000000
    //  ┌───────────┬───────────────┬──────────────┬──────────────────────────────────────────────────────┬──────────┐
    //  │ SC & 0x81 │ bit7 (armed?) │ bit0 (clock) │ meaning                                              │ feed it? │
    //  ├───────────┼───────────────┼──────────────┼──────────────────────────────────────────────────────┼──────────┤
    //  │ 0x00      │ no            │ external     │ idle                                                 │ no       │
    //  ├───────────┼───────────────┼──────────────┼──────────────────────────────────────────────────────┼──────────┤
    //  │ 0x01      │ no            │ internal     │ idle                                                 │ no       │
    //  ├───────────┼───────────────┼──────────────┼──────────────────────────────────────────────────────┼──────────┤
    //  │ 0x80      │ yes           │ external     │ armed slave — mGB waiting for us                     │ YES      │
    //  ├───────────┼───────────────┼──────────────┼──────────────────────────────────────────────────────┼──────────┤
    //  │ 0x81      │ yes           │ internal     │ armed master (GB drives clock; Printer/Workboy case) │ no       │
    //  └───────────┴───────────────┴──────────────┴──────────────────────────────────────────────────────┴──────────┘
    if ((gb->io_registers[GB_IO_SC] & 0x81) != 0x80) {
        return;
    }

    // idle
    if (gb->midi.bits_left == 0) {
        // if the buffer is empty, there is nothing do do
        if (gb->midi.queue_head == gb->midi.queue_tail) {
            return;
        }

        // Otherwise, we prepare the next byte
        gb->midi.byte_being_sent = gb->midi.queue[gb->midi.queue_head];
        gb->midi.queue_head = gb->midi.queue_head + 1;
        gb->midi.bits_left = 8;
    }

    // send most significant bit
    GB_serial_set_data_bit(gb, gb->midi.byte_being_sent >> 7);
    // move the next bit to the MSB position and substract one from the bits left to send
    gb->midi.byte_being_sent <<= 1;
    gb->midi.bits_left--;
}