/* Volume above 100%: louder without crackle. The sound is turned up evenly,
   and a fast limiter pulls back only on the loudest moments, then eases off
   gently, so peaks never clip and nothing sounds squashed or fuzzy. */
#include "nest.h"

#define CEIL 30500.0f /* about -0.6 dB: headroom so the speaker never clips */

void boost_apply(int16_t *buf, int frames, int ch, float gain, float *state) {
    float g = *state > 0 ? *state : 1.0f;      /* limiter gain, 1 = untouched */
    const float release = 0.00035f;            /* ~60 ms back to full at 48 kHz */
    for (int i = 0; i < frames; i++) {
        int16_t *s = buf + i * ch;
        float l = s[0] * gain, r = ch > 1 ? s[1] * gain : l;
        float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        float want = pk > CEIL ? CEIL / pk : 1.0f;
        if (want < g) g = want;                 /* instant: never let a peak through */
        else g += (want - g) * release;         /* slow: no pumping or wobble */
        l *= g;
        r *= g;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        s[0] = (int16_t)l;
        if (ch > 1) s[1] = (int16_t)r;
    }
    *state = g;
}
