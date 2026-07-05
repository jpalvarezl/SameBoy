/*
 * White-box unit test for the MIDI input ring buffer (Task 1: GB_midi_input_byte).
 *
 * This is a throwaway dev harness, not part of the build. It links directly against
 * SameBoy's already-compiled core object files, so there's no Makefile involvement.
 * Rebuild the core first (so the .o files are fresh), then build and run this with:
 *
 *   `clang -DGB_INTERNAL -I. -g -o build/bin/test/midi_test TestUtils/midi_test.c build/obj/Core/\*.o`
 *
 * (replace [star] with a literal asterisk -- spelled out here only so this comment
 *  doesn't contain a slash-star sequence, which would trip a -Wcomment warning.)
 *
 *   -DGB_INTERNAL       exposes the full GB_gameboy_t struct so we can inspect gb.midi.*
 *   -I.                 lets <Core/gb.h> resolve from the repo root.
 *   the core .o objects  supply GB_init/GB_connect_midi/etc.; none of them define main().
 *
 * "White-box" = the test reaches into internal state (gb.midi.queue_head/tail/queue[])
 * to check the data structure directly. That's appropriate here because Task 1 has no
 * consumer yet to observe behavior through. Once GB_midi_run exists (Task 2) we can add
 * black-box tests that only look at observable output.
 */
#include <Core/gb.h>
#include <stdio.h>

/*
 * A tiny check helper. Unlike assert(), it doesn't abort on the first failure -- it
 * records the failure and keeps going, so one run reports *every* broken expectation.
 * #cond stringifies the expression; __LINE__ points you at the offending check.
 * The do { ... } while (0) wrapper is the standard idiom that lets a multi-statement
 * macro be used like a single statement (e.g. inside an if without braces).
 */
static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL (line %d): %s\n", __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* Bytes currently queued. Unsigned 8-bit subtraction wraps mod 256, so this is correct
 * even when tail has wrapped past head. Handy for asserting the buffer's fill level. */
static uint8_t queued_count(const GB_gameboy_t *gb)
{
    return (uint8_t)(gb->midi.queue_tail - gb->midi.queue_head);
}

/* Reset the ring to empty between independent test cases. */
static void reset_queue(GB_gameboy_t *gb)
{
    gb->midi.queue_head = 0;
    gb->midi.queue_tail = 0;
}

int main(void)
{
    GB_gameboy_t gb;
    GB_init(&gb, GB_MODEL_DMG_B);
    GB_connect_midi(&gb);

    /* --- 1. Connecting the device leaves an empty queue and the right accessory. --- */
    CHECK(gb.accessory == GB_ACCESSORY_MIDI);
    CHECK(gb.midi.queue_head == gb.midi.queue_tail);   /* head == tail  <=>  empty */
    CHECK(queued_count(&gb) == 0);

    /* --- 2. Basic enqueue: bytes land in order, tail advances, head does not. --- */
    reset_queue(&gb);
    const uint8_t sample[] = {0x90, 0x3C, 0x7F, 0x80}; /* note-on, note, velocity, note-off */
    for (size_t i = 0; i < sizeof(sample); i++) {
        GB_midi_input_byte(&gb, sample[i]);
    }
    CHECK(queued_count(&gb) == sizeof(sample));
    CHECK(gb.midi.queue_head == 0);                    /* producer must never move head */
    CHECK(gb.midi.queue_tail == sizeof(sample));
    bool order_ok = true;
    for (size_t i = 0; i < sizeof(sample); i++) {
        if (gb.midi.queue[i] != sample[i]) order_ok = false;
    }
    CHECK(order_ok);

    /* --- 3. Guard: input is ignored when no MIDI device is attached. --- */
    reset_queue(&gb);
    gb.accessory = GB_ACCESSORY_NONE;                  /* pretend it's disconnected */
    GB_midi_input_byte(&gb, 0x42);
    CHECK(queued_count(&gb) == 0);                     /* nothing enqueued */
    gb.accessory = GB_ACCESSORY_MIDI;                  /* restore for the next cases */

    /* --- 4. The important off-by-one: capacity is 255, not 256, and a full buffer
     *        drops extra bytes WITHOUT corrupting head (which would look like data loss). --- */
    reset_queue(&gb);
    for (int i = 0; i < 300; i++) {
        GB_midi_input_byte(&gb, (uint8_t)i);           /* push way more than fits */
    }
    CHECK(queued_count(&gb) == 255);                   /* one slot reserved to tell empty from full */
    CHECK(gb.midi.queue_head == 0);                    /* head untouched: no reader ran */
    bool kept_first_255 = true;                        /* the accepted bytes are the FIRST 255 */
    for (int i = 0; i < 255; i++) {
        if (gb.midi.queue[i] != (uint8_t)i) kept_first_255 = false;
    }
    CHECK(kept_first_255);

    /* --- 5. Wrap-around: writing past index 255 must loop back to 0 correctly.
     *        We simulate a partly-drained ring by hand-placing head/tail near the end. --- */
    gb.midi.queue_head = 250;
    gb.midi.queue_tail = 250;
    for (int i = 0; i < 10; i++) {
        GB_midi_input_byte(&gb, (uint8_t)(0xA0 + i));  /* indices 250..255 then 0..3 */
    }
    CHECK(queued_count(&gb) == 10);
    CHECK(gb.midi.queue_tail == 4);                    /* 250 + 10 = 260 -> wraps to 4 */
    bool wrap_ok = true;
    for (int i = 0; i < 10; i++) {
        uint8_t idx = (uint8_t)(250 + i);              /* wraps through 0 automatically */
        if (gb.midi.queue[idx] != (uint8_t)(0xA0 + i)) wrap_ok = false;
    }
    CHECK(wrap_ok);

    GB_free(&gb);

    if (g_failures == 0) {
        printf("ALL MIDI QUEUE TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
