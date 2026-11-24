#include <stdio.h>
#include <stdint.h>
#include "audio_pts_clock.h"

static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    audio_pts_state s = {0};
    const uint64_t origin = 1000000u;
    CHECK(audio_pts_seed(&s, origin, 7, 7));
    CHECK(s.valid && s.read_us == origin && s.write_us == origin);
    // Every partition of 48 kHz sample time must preserve the same clock.
    for (unsigned i = 0; i < 675000u; ++i)
        audio_pts_advance_read(&s, 256u);
    CHECK(s.read_us == origin + 3600000000ULL);
    CHECK(s.read_remainder == 0);
    printf("1-hour 256-frame block clock: %llu us; old truncation error: 225000 us\n",
           (unsigned long long)(s.read_us-origin));

    audio_pts_reset(&s);
    CHECK(audio_pts_seed(&s, origin, 9, 9));
    uint64_t total = 0;
    for (unsigned i = 0; i < 100000u; ++i) {
        const unsigned n = (i * 97u) % 4096u + 1u;
        total += n;
        audio_pts_advance_read(&s, n);
        audio_pts_advance_write(&s, n);
        CHECK(s.read_us == origin + total*125u/6u);
        CHECK(s.write_us == s.read_us);
        CHECK(s.read_remainder == total*125u%6u);
        CHECK(s.write_remainder == s.read_remainder);
    }

    // Each PES may re-anchor the writer; it must not re-anchor the reader.
    const uint64_t old_read = s.read_us;
    const unsigned old_rem = s.read_remainder;
    CHECK(audio_pts_seed(&s, 123456u, 9, 9));
    CHECK(s.write_us == 123456u && s.write_remainder == 0);
    CHECK(s.read_us == old_read && s.read_remainder == old_rem);

    // Seek/track flush resets both clocks, and stale in-flight packets cannot
    // set valid or re-seed either cursor after the new generation starts.
    audio_pts_reset(&s);
    CHECK(!audio_pts_seed(&s, 987654321u, 9, 10));
    CHECK(!s.valid && s.read_us == 0 && s.write_us == 0);
    CHECK(audio_pts_seed(&s, 333u, 10, 10));
    audio_pts_advance_read(&s, 1);
    CHECK(s.read_us == 353u && s.read_remainder == 5u);
    CHECK(!audio_pts_seed(&s, 123u, 9, 10));
    CHECK(s.read_us == 353u && s.write_us == 333u);
    audio_pts_reset(&s);
    CHECK(s.read_remainder == 0 && s.write_remainder == 0 && !s.valid);
    printf("audio PTS clock: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
