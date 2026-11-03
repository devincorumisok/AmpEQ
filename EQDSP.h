#ifndef AMPEQ_DSP_H
#define AMPEQ_DSP_H
#include <stdint.h>
#define EQ_BANDS 12
#define EQ_CHANNELS 8
extern const float eq_freqs[EQ_BANDS];
#define EQ_Q 1.20f
typedef struct {float b0,b1,b2,a1,a2;} EQCoeff;
typedef struct {float z1,z2;} EQHistory;
typedef struct {
 EQCoeff coeff[EQ_BANDS]; EQHistory hist[EQ_CHANNELS][EQ_BANDS];
 float gain;        /* linear output gain (preamp + auto headroom) */
 float xgain;       /* extra linear gain (clipping protection), 1 = none */
 float peak;        /* largest |sample| before the limiter since last read (meter) */
 float sample_rate; int active; int nbands; uint8_t band_on[EQ_BANDS];
} EQEngine;

/* Peak magnitude (dB) of the 12-band cascade over 20 Hz..20 kHz at `rate`. */
float eq_peak_db(const float gains[EQ_BANDS], float rate);
/* Magnitude response (dB) of the cascade at frequency f. */
float eq_response_db(const float gains[EQ_BANDS], float rate, float f);
/* Total output gain in dB = user preamp - max(0, peak). Never boosts past 0 dBFS
 * from the EQ curve itself; a positive user preamp is allowed and hits the limiter. */
float eq_auto_gain_db(const float gains[EQ_BANDS], float preamp_db, float rate);
/* Fit 12 band gains so the cascade matches target_db[] at the band centres
 * (compensates for overlap between neighbouring peaking filters). */
void eq_fit(const float target_db[EQ_BANDS], float out_gains[EQ_BANDS]);

/* Update coefficients WITHOUT clearing filter history (no click on change). */
void eq_set(EQEngine *e, float rate, const float gains[EQ_BANDS], float total_gain_db, int enabled);
void eq_reset_history(EQEngine *e);
/* compat with v1.4.2 tests: eq_set + reset, total gain computed automatically */
void eq_configure(EQEngine *e, float rate, const float gains[EQ_BANDS], float preamp_db, int enabled);

float eq_sample(EQEngine *e, unsigned channel, float x);
void eq_process_float(EQEngine *e, float *p, uint32_t frames, uint32_t stride, unsigned ch);
void eq_process_s16(EQEngine *e, int16_t *p, uint32_t frames, uint32_t stride, unsigned ch);
/* 32-bit signed integer; frac_bits = 24 for 8.24 fixed point, 31 for plain s32 */
void eq_process_s32(EQEngine *e, int32_t *p, uint32_t frames, uint32_t stride, unsigned ch, unsigned frac_bits);
#endif
