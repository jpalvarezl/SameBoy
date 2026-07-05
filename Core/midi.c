#include "gb.h"
#include <string.h>

static void serial_start(GB_gameboy_t *gb, bool bit_received) { 
    // TODO: protocol hook
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
    
}