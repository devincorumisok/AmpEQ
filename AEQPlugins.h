/* AmpEQ plugins: stereo effects that run after the 12-band EQ, inside mediaserverd.
 * Modeled on the controls of HiBy's plugins (their code is closed); these are AmpEQ's own
 * implementations of the same ideas. Pure C, no allocation in the audio path. */
#ifndef AEQ_PLUGINS_H
#define AEQ_PLUGINS_H
#include <stdint.h>

enum { PLG_DRX=1<<0, PLG_GXFEED=1<<1, PLG_XFEED=1<<2, PLG_FIELD=1<<3, PLG_PITCH=1<<4, PLG_CONV=1<<5, PLG_BALANCE=1<<6,
       PLG_PITCH_LIMIT=1<<7, PLG_CONV_BYPASS_OTHER=1<<8 };

typedef struct {
 uint32_t flags;
 float drx_bass, drx_mids, drx_treble, drx_gain_db;   /* -1.0..2.0 x (0.1 steps), -10..+5 dB */
 float gx_bass_db, gx_mids_db, gx_delay_ms, gx_bass_hz, gx_mids_hz, gx_gain_db;
 int   xfeed_level;                                   /* 0 low, 1 medium, 2 high */
 float field_width;                                   /* 0..2 (1 = unchanged) */
 float pitch_gain_db;                                 /* -10..0 dB */
 float conv_level;                                    /* -20..+20 dB level of the convolved signal */
 float balance;                                       /* -100..100 */
 uint32_t conv_serial;                                /* bump -> reload IR file */
} AEQPluginParams;

/* wire format: 4 notify words */
void aeq_plg_pack(const AEQPluginParams *p, uint64_t w[4]);
void aeq_plg_unpack(const uint64_t w[4], AEQPluginParams *p);

/* ---- convolution impulse response (built off the audio thread) ---- */
typedef struct AEQIR AEQIR;
/* frames: interleaved stereo float32 at `rate`; returns NULL on failure. Energy-normalised. */
AEQIR *aeq_ir_create(const float *stereo, uint32_t frames, uint32_t rate);
void aeq_ir_free(AEQIR *ir);
uint32_t aeq_ir_rate(const AEQIR *ir);
#define AEQ_IR_FILE_FMT "/var/jb/var/mobile/Library/AmpEQ/ir_%u.f32"   /* header: 'AEQI', rate, frames, 2 */
#define AEQ_IR_MAX_FRAMES (2*48000)
#define AEQ_CONV_BLOCK 256

/* ---- processing state ---- */
typedef struct AEQPluginState AEQPluginState;
AEQPluginState *aeq_plg_state_create(void);                /* allocate once, off the audio thread */
/* audio thread: L,R float pointers with stride (1 = non-interleaved, 2 = interleaved) */
void aeq_plg_process(AEQPluginState *s, const AEQPluginParams *p, const AEQIR *ir, float rate,
                     float *L, float *R, uint32_t stride, uint32_t n);
void aeq_plg_reset(AEQPluginState *s);
#endif
