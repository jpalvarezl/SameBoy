// Unit tests for the MIDI input accessory (Core/midi.c).
//
// Throwaway dev harness, not part of the build. It links directly against SameBoy's
// already-compiled core object files, so there's no Makefile involvement. Rebuild the
// core first (so the .o files are fresh), then build and run this with:
//
//   clang -DGB_INTERNAL -I. -g -o build/bin/test/midi_test TestUtils/midi_test.c build/obj/Core/*.o
//   ./build/bin/test/midi_test
//
//   -DGB_INTERNAL        exposes the full GB_gameboy_t struct so we can inspect gb.midi.*
//   -I.                  lets <Core/gb.h> resolve from the repo root.
//   the core .o objects  supply GB_init/GB_connect_midi/etc.; none of them define main().
//
// Two testing styles live here:
//   * white-box: reach into internal state (gb.midi.queue_*) to check the ring buffer directly.
//   * black-box: feed bytes in and observe the serial port (SB/SC/IF) -- see the Task 2 tests.

#include <Core/gb.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tiny zero-dependency test harness.
//
// CHECK records a failure and keeps going (unlike assert, which aborts on the first),
// so one run reports every broken expectation. #cond stringifies the expression and
// g_test/__LINE__ tell you which test and line failed. RUN records the current test
// name, then calls it. do { ... } while (0) lets a multi-statement macro act as one.
// ---------------------------------------------------------------------------
static int g_failures = 0;
static const char *g_test = "";

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define RUN(test) do { g_test = #test; test(); } while (0)

// Fresh, MIDI-connected Game Boy for a single test. Pair every setup() with GB_free().
static void setup(GB_gameboy_t *gb)
{
    GB_init(gb, GB_MODEL_DMG_B);
    GB_connect_midi(gb);
}

// Bytes currently queued. Unsigned 8-bit subtraction wraps mod 256, so this is correct
// even when tail has wrapped past head.
static uint8_t queued_count(const GB_gameboy_t *gb)
{
    return (uint8_t)(gb->midi.queue_tail - gb->midi.queue_head);
}

// ===========================================================================
// Task 1 -- the ring buffer (white-box: inspects gb.midi.queue_* directly)
// ===========================================================================

// Connecting the device leaves an empty queue and the right accessory.
static void test_empty_on_connect(void)
{
    GB_gameboy_t gb;
    setup(&gb);
    CHECK(gb.accessory == GB_ACCESSORY_MIDI);
    CHECK(gb.midi.queue_head == gb.midi.queue_tail);   // head == tail  <=>  empty
    CHECK(queued_count(&gb) == 0);
    GB_free(&gb);
}

// Bytes land in order, tail advances, head does not.
static void test_basic_enqueue(void)
{
    GB_gameboy_t gb;
    setup(&gb);
    const uint8_t sample[] = {0x90, 0x3C, 0x7F, 0x80}; // note-on, note, velocity, note-off
    for (size_t i = 0; i < sizeof(sample); i++) {
        GB_midi_input_byte(&gb, sample[i]);
    }
    CHECK(queued_count(&gb) == sizeof(sample));
    CHECK(gb.midi.queue_head == 0);                    // producer must never move head
    CHECK(gb.midi.queue_tail == sizeof(sample));
    bool order_ok = true;
    for (size_t i = 0; i < sizeof(sample); i++) {
        if (gb.midi.queue[i] != sample[i]) order_ok = false;
    }
    CHECK(order_ok);
    GB_free(&gb);
}

// Input is ignored when no MIDI device is attached.
static void test_guard_when_disconnected(void)
{
    GB_gameboy_t gb;
    setup(&gb);
    gb.accessory = GB_ACCESSORY_NONE;                  // pretend it's disconnected
    GB_midi_input_byte(&gb, 0x42);
    CHECK(queued_count(&gb) == 0);                     // nothing enqueued
    GB_free(&gb);
}

// The important off-by-one: capacity is 255, not 256, and a full buffer drops extra
// bytes WITHOUT corrupting head (which would look like silent data loss).
static void test_full_buffer_drops(void)
{
    GB_gameboy_t gb;
    setup(&gb);
    for (int i = 0; i < 300; i++) {
        GB_midi_input_byte(&gb, (uint8_t)i);           // push way more than fits
    }
    CHECK(queued_count(&gb) == 255);                   // one slot reserved to tell empty from full
    CHECK(gb.midi.queue_head == 0);                    // head untouched: no reader ran
    bool kept_first_255 = true;                        // the accepted bytes are the FIRST 255
    for (int i = 0; i < 255; i++) {
        if (gb.midi.queue[i] != (uint8_t)i) kept_first_255 = false;
    }
    CHECK(kept_first_255);
    GB_free(&gb);
}

// Writing past index 255 must loop back to 0. We hand-place head/tail near the end to
// force the wrap without needing a consumer (this is test-specific setup, not a reset).
static void test_wraparound(void)
{
    GB_gameboy_t gb;
    setup(&gb);
    gb.midi.queue_head = 250;
    gb.midi.queue_tail = 250;
    for (int i = 0; i < 10; i++) {
        GB_midi_input_byte(&gb, (uint8_t)(0xA0 + i));  // indices 250..255 then 0..3
    }
    CHECK(queued_count(&gb) == 10);
    CHECK(gb.midi.queue_tail == 4);                    // 250 + 10 = 260 -> wraps to 4
    bool wrap_ok = true;
    for (int i = 0; i < 10; i++) {
        uint8_t idx = (uint8_t)(250 + i);              // wraps through 0 automatically
        if (gb.midi.queue[idx] != (uint8_t)(0xA0 + i)) wrap_ok = false;
    }
    CHECK(wrap_ok);
    GB_free(&gb);
}

// ===========================================================================
// Task 2 -- clocking bytes into the GB (black-box: observe SB/SC/IF)
// ===========================================================================
// TODO(you): add the behavioral tests here, then register them with RUN() below.
// A helper like deliver_one_byte(gb) -- arm SC, clear IF, call GB_midi_run 8x -- will help.
static void deliver_one_byte(GB_gameboy_t *gb)
{
    gb->io_registers[GB_IO_SC] = 0x80;              // arm: start=1, external clock=0
    gb->io_registers[GB_IO_IF] = 0;                 // clear so we can see the serial IRQ fire
    for (int i = 0; i < 8; i++) GB_midi_run(gb);    // 8 ticks = 8 bits
}

static void test_byte_arrives_intact(void)
{
    GB_gameboy_t gb; 
    setup(&gb);
    GB_midi_input_byte(&gb, 0x90);

    deliver_one_byte(&gb);
    CHECK(gb.io_registers[GB_IO_SB] == 0x90); // full value transfered
    CHECK((gb.io_registers[GB_IO_SC] & 0x80) == 0); // disarmed; transfer done
    CHECK((gb.io_registers[GB_IO_IF] & 0x08) != 0); // IRQ raised

    GB_free(&gb);
}

// Pacing (the important one): an un-armed slave must never be overrun. We deliver one byte
// so SB holds a known value, then queue another but leave SC un-armed -- the heartbeat must
// clock nothing while the slave isn't ready, and the byte must stay safely queued. This is
// the property that makes clocking a slave safe: SC bit7 IS the back-pressure signal.
static void test_pacing_no_overrun(void)
{
    GB_gameboy_t gb;
    setup(&gb);

    // Baseline: first byte arrives normally, so SB holds a known 0x90.
    GB_midi_input_byte(&gb, 0x90);
    deliver_one_byte(&gb);
    CHECK(gb.io_registers[GB_IO_SB] == 0x90);

    // Queue a second byte, but mGB has NOT re-armed (SC bit7 clear). Pump the heartbeat hard:
    // not a single bit may be clocked while the slave is un-armed.
    GB_midi_input_byte(&gb, 0x3C);
    gb.io_registers[GB_IO_SC] = 0x00;               // un-armed
    for (int i = 0; i < 20; i++) GB_midi_run(&gb);

    CHECK(gb.io_registers[GB_IO_SB] == 0x90);       // unchanged: nothing was clocked
    CHECK(queued_count(&gb) == 1);                  // the byte is still waiting, not lost

    GB_free(&gb);
}

static void test_sequential_delivery(void)
{
    GB_gameboy_t gb; 
    setup(&gb);
    GB_midi_input_byte(&gb, 0x3C);
    GB_midi_input_byte(&gb, 0x7F);

    deliver_one_byte(&gb);
    CHECK(gb.io_registers[GB_IO_SB] == 0x3C); // full value transfered
    deliver_one_byte(&gb);
    CHECK(gb.io_registers[GB_IO_SB] == 0x7F); // full value transfered
    CHECK((gb.io_registers[GB_IO_SC] & 0x80) == 0); // disarmed; transfer done
    CHECK((gb.io_registers[GB_IO_IF] & 0x08) != 0); // IRQ raised

    GB_free(&gb);
}

static void test_empty_queue_is_safe(void)
{
    GB_gameboy_t gb; 
    setup(&gb);
    uint8_t initial_sb_value = gb.io_registers[GB_IO_SB];

    deliver_one_byte(&gb);   // arms SC, but the queue is empty -> nothing should be clocked
    CHECK(gb.io_registers[GB_IO_SB] == initial_sb_value); // SB untouched
    CHECK((gb.io_registers[GB_IO_SC] & 0x80) != 0);       // STILL armed: no byte completed
    CHECK((gb.io_registers[GB_IO_IF] & 0x08) == 0);       // no serial interrupt raised

    GB_free(&gb);
}

// ===========================================================================
// Phase 3 / MIDI out -- capturing bytes the GB clocks out (serial_start).
//
// Here the GB is the MASTER: normally the core's serial machinery calls serial_start
// once per outgoing bit. We stand in for that by invoking the *registered* callback
// pointer directly with a known bit pattern (MSB-first, exactly how the hardware shifts),
// then check that serial_start reassembles the original byte.
//
// serial_start currently reports each completed byte via GB_log("MIDI out: %02X"), so we
// route the log into a buffer and read it back. When the Task O2 output callback lands,
// swap capture_log for that callback -- the assertions stay the same.
// ===========================================================================

static char   g_log[4096];
static size_t g_log_len;

static void reset_log(void)
{
    g_log_len = 0;
    g_log[0] = '\0';
}

// GB_log sink: append every logged string so tests can grep it.
static void capture_log(GB_gameboy_t *gb, const char *string, GB_log_attributes_t attributes)
{
    (void)gb; (void)attributes;
    size_t n = strlen(string);
    if (g_log_len + n < sizeof(g_log)) {
        memcpy(g_log + g_log_len, string, n);
        g_log_len += n;
        g_log[g_log_len] = '\0';
    }
}

// setup() plus a log sink, so completed out-bytes are readable via g_log.
static void setup_capturing(GB_gameboy_t *gb)
{
    setup(gb);
    GB_set_log_callback(gb, capture_log);
    reset_log();
}

// Clock one byte OUT of the GB, MSB-first, through the registered start callback.
static void clock_out_byte(GB_gameboy_t *gb, uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        gb->serial_transfer_bit_start_callback(gb, (byte >> i) & 1);
    }
}

// Eight bits in, MSB-first, reassemble to the exact byte.
static void test_out_byte_assembled_msb_first(void)
{
    GB_gameboy_t gb;
    setup_capturing(&gb);
    clock_out_byte(&gb, 0x90);
    CHECK(strstr(g_log, "MIDI out: 90") != NULL);
    GB_free(&gb);
}

// A partial byte (7 bits) must NOT emit -- the accumulator waits for the 8th bit.
static void test_out_partial_byte_not_emitted(void)
{
    GB_gameboy_t gb;
    setup_capturing(&gb);
    for (int i = 7; i >= 1; i--) {
        gb.serial_transfer_bit_start_callback(&gb, (0x90 >> i) & 1);  // only 7 bits
    }
    CHECK(gb.midi.bits_received == 7);            // mid-byte
    CHECK(strstr(g_log, "MIDI out") == NULL);     // nothing completed yet
    GB_free(&gb);
}

// Consecutive bytes: the accumulator resets between them and each value is exact.
static void test_out_resets_between_bytes(void)
{
    GB_gameboy_t gb;
    setup_capturing(&gb);
    clock_out_byte(&gb, 0x3C);
    clock_out_byte(&gb, 0x7F);
    CHECK(gb.midi.bits_received == 0);            // clean slate after each byte
    CHECK(gb.midi.byte_being_received == 0);
    CHECK(strstr(g_log, "MIDI out: 3C") != NULL);
    CHECK(strstr(g_log, "MIDI out: 7F") != NULL);
    GB_free(&gb);
}

int main(void)
{
    RUN(test_empty_on_connect);
    RUN(test_basic_enqueue);
    RUN(test_guard_when_disconnected);
    RUN(test_full_buffer_drops);
    RUN(test_wraparound);
    RUN(test_byte_arrives_intact);
    RUN(test_pacing_no_overrun);
    RUN(test_sequential_delivery);
    RUN(test_empty_queue_is_safe);

    RUN(test_out_byte_assembled_msb_first);
    RUN(test_out_partial_byte_not_emitted);
    RUN(test_out_resets_between_bytes);

    if (g_failures == 0) {
        printf("ALL MIDI TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
