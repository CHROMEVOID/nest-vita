/* Music sound: a 6-band equaliser, a "loud" mode for quiet songs, the volume
   boost, and a limiter at the end so nothing ever clips. Everything runs in
   floating point on the music thread. */
#include "nest.h"
#include "music.h"

const int EQ_FREQ[EQ_BANDS] = {60, 170, 450, 1200, 3500, 10000};
const char *EQ_LABEL[EQ_BANDS] = {"60", "170", "450", "1.2k", "3.5k", "10k"};

const EqPreset EQ_PRESETS[EQ_PRESET_COUNT] = {
    {"Flat", {0, 0, 0, 0, 0, 0}},
    {"Bass boost", {7, 5, 1, 0, 0, 0}},
    {"Lofi warm", {3, 3, 1, -1, -3, -5}},
    {"Hip-hop", {6, 4, -1, 0, 2, 3}},
    {"Vocals", {-2, -1, 2, 4, 3, 0}},
    {"Treble", {0, 0, 0, 1, 4, 6}},
    {"Rock", {4, 2, -2, 0, 3, 4}},
    {"Acoustic", {2, 1, 0, 2, 2, 2}},
    {"Late night", {2, 1, 0, -1, -2, -3}},
#ifdef NEST_PORT
    {"Handheld speaker", {-6, 0, 3, 3, 2, 1}},
#else
    {"Vita speaker", {-6, 0, 3, 3, 2, 1}},
#endif
    /* ^ the little speakers can't do deep bass: give it to the mids */
};

/* settings shared with the music thread */
static volatile float eq_gain[EQ_BANDS];
static volatile int eq_on, loud_on, eq_dirty = 1;
static int preset_i;

void eq_set_band(int b, float db) {
    if (b < 0 || b >= EQ_BANDS) return;
    eq_gain[b] = clampf(db, -12, 12);
    eq_dirty = 1;
}
float eq_band(int b) { return b >= 0 && b < EQ_BANDS ? eq_gain[b] : 0; }
void eq_enable(int on) { eq_on = on; eq_dirty = 1; }
int eq_enabled(void) { return eq_on; }
void eq_set_loud(int on) { loud_on = on; }
int eq_loud(void) { return loud_on; }
int eq_preset(void) { return preset_i; }

void eq_apply_preset(int p) {
    if (p < 0 || p >= EQ_PRESET_COUNT) return;
    preset_i = p;
    for (int b = 0; b < EQ_BANDS; b++) eq_gain[b] = EQ_PRESETS[p].db[b];
    eq_on = 1;
    eq_dirty = 1;
}

/* -1 when the sliders don't match a preset ("Custom") */
int eq_matching_preset(void) {
    for (int p = 0; p < EQ_PRESET_COUNT; p++) {
        int ok = 1;
        for (int b = 0; b < EQ_BANDS; b++)
            if (fabsf(eq_gain[b] - EQ_PRESETS[p].db[b]) > 0.25f) ok = 0;
        if (ok) return p;
    }
    return -1;
}

void eq_save(void) {
    char v[96];
    snprintf(v, sizeof(v), "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f", eq_gain[0], eq_gain[1], eq_gain[2], eq_gain[3], eq_gain[4], eq_gain[5]);
    db_set("eq_bands", v);
    db_seti("eq_on", eq_on);
    db_seti("eq_loud", loud_on);
    db_seti("eq_preset", preset_i);
}

void eq_load(void) {
    const char *v = db_get("eq_bands", NULL);
    float g[EQ_BANDS] = {0};
    if (v) sscanf(v, "%f,%f,%f,%f,%f,%f", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5]);
    for (int b = 0; b < EQ_BANDS; b++) eq_gain[b] = clampf(g[b], -12, 12);
    eq_on = db_geti("eq_on", 0);
    loud_on = db_geti("eq_loud", 0);
    preset_i = clampi(db_geti("eq_preset", 0), 0, EQ_PRESET_COUNT - 1);
    eq_dirty = 1;
}

/* ---------------- the filters (audio thread) ---------------- */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2];
} Biquad;

static Biquad bq[EQ_BANDS];
static int bq_rate;
static float preamp = 1;

/* RBJ cookbook: low shelf for the first band, high shelf for the last, peaks between */
static void design(Biquad *f, int type, float fs, float f0, float db, float q) {
    float A = powf(10, db / 40), w = 2 * (float)M_PI * f0 / fs, cw = cosf(w), sw = sinf(w);
    float b0, b1, b2, a0, a1, a2;
    if (type == 0 || type == 2) {
        float S = 0.9f, alpha = sw / 2 * sqrtf((A + 1 / A) * (1 / S - 1) + 2), sa = 2 * sqrtf(A) * alpha;
        if (type == 0) {
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
        }
    } else {
        float alpha = sw / (2 * q);
        b0 = 1 + alpha * A;
        b1 = -2 * cw;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cw;
        a2 = 1 - alpha / A;
    }
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

static void rebuild(int rate) {
    float maxboost = 0;
    for (int b = 0; b < EQ_BANDS; b++) {
        int type = b == 0 ? 0 : b == EQ_BANDS - 1 ? 2 : 1;
        /* shelves sit a little inside the band so 60 Hz / 10 kHz get the full amount */
        float f0 = type == 0 ? EQ_FREQ[b] * 1.8f : type == 2 ? EQ_FREQ[b] * 0.65f : EQ_FREQ[b];
        if (f0 > rate * 0.45f) f0 = rate * 0.45f;
        design(&bq[b], type, (float)rate, f0, eq_gain[b], 1.0f);
        if (eq_gain[b] > maxboost) maxboost = eq_gain[b];
    }
    /* make room for the boosted bands, so the EQ itself doesn't push into the limiter all the time */
    preamp = powf(10, -maxboost * 0.5f / 20);
    bq_rate = rate;
    eq_dirty = 0;
}

void dsp_run(int16_t *buf, int frames, int rate, float gain, DspState *st) {
    int use_eq = eq_on;
    if (use_eq && (eq_dirty || bq_rate != rate)) rebuild(rate);
    int loud = loud_on;
    if (!use_eq && !loud && gain == 1.0f) {
        st->lim = 1;
        return;
    }
    float g = gain * (use_eq ? preamp : 1);
    if (loud) g *= 1.8f; /* about +5 dB, the limiter keeps it clean */
    float lim = st->lim > 0 ? st->lim : 1;
    /* loud mode lets the limiter recover a little slower, so it sounds even rather than pumping */
    const float rel = loud ? 0.00018f : 0.00035f;
    const float ceil_ = 30500.0f;
    for (int i = 0; i < frames; i++) {
        float x[2] = {buf[i * 2], buf[i * 2 + 1]};
        if (use_eq) {
            for (int b = 0; b < EQ_BANDS; b++) {
                Biquad *f = &bq[b];
                for (int c = 0; c < 2; c++) {
                    float y = f->b0 * x[c] + f->z1[c];
                    f->z1[c] = f->b1 * x[c] - f->a1 * y + f->z2[c];
                    f->z2[c] = f->b2 * x[c] - f->a2 * y;
                    x[c] = y;
                }
            }
        }
        x[0] *= g;
        x[1] *= g;
        float pk = fmaxf(fabsf(x[0]), fabsf(x[1]));
        float want = pk > ceil_ ? ceil_ / pk : 1.0f;
        if (want < lim) lim = want;
        else lim += (want - lim) * rel;
        for (int c = 0; c < 2; c++) {
            float v = x[c] * lim;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            buf[i * 2 + c] = (int16_t)v;
        }
    }
    /* flush tiny numbers so the filters never slow down on silence */
    for (int b = 0; b < EQ_BANDS; b++)
        for (int c = 0; c < 2; c++) {
            if (fabsf(bq[b].z1[c]) < 1e-15f) bq[b].z1[c] = 0;
            if (fabsf(bq[b].z2[c]) < 1e-15f) bq[b].z2[c] = 0;
        }
    st->lim = lim;
}
