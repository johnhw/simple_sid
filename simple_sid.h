#ifndef SID_EMU_H
#define SID_EMU_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

/* ------------------------------------------------------------------
   If your environment doesn't define M_PI:
   ------------------------------------------------------------------ */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float low;
    float band;
} filterState_t;

/* ------------------------------------------------------------------
   ADSR (Attack/Decay/Release) states
   ------------------------------------------------------------------ */
typedef enum
{
    ATTACK = 0,
    DECAY,
    RELEASE
} adsrState_t;

/* ------------------------------------------------------------------
   One SID channel
   ------------------------------------------------------------------ */
typedef struct sidChannel_s
{
    uint16_t frequency;
    uint8_t ad; /* Attack= high nibble, Decay= low nibble */
    uint8_t sr; /* Sustain= high nibble, Release= low nibble */
    uint16_t pulse;
    uint8_t waveform;
    bool doSync;
    adsrState_t state;
    unsigned accumulator;    /* 24-bit, in a 32-bit unsigned */
    unsigned noiseGenerator; /* up to 23 bits used */
    uint16_t adsrCounter;
    uint8_t adsrExpCounter;
    uint8_t volumeLevel;

    struct sidChannel_s *syncTarget;
    struct sidChannel_s *syncSource;
} sidChannel_t;

/* ------------------------------------------------------------------
   The SID chip itself: 3 channels + filter state + sample stepping
   ------------------------------------------------------------------ */
typedef struct
{
    sidChannel_t channels[3];
    float cyclesPerSample;
    float cycleAccumulator;
    filterState_t filter;
    
} sid_t;

/* ------------------------------------------------------------------
   The struct containing register values for each channel and filter.
   For 2-byte fields, use int16_t; for 1-byte fields, use int8_t.
   ------------------------------------------------------------------ */
typedef struct
{
    /* Channel 0 */
    int16_t freq0;
    int16_t pulse0;
    int8_t waveform0;
    int8_t ad0;
    int8_t sr0;

    /* Channel 1 */
    int16_t freq1;
    int16_t pulse1;
    int8_t waveform1;
    int8_t ad1;
    int8_t sr1;

    /* Channel 2 */
    int16_t freq2;
    int16_t pulse2;
    int8_t waveform2;
    int8_t ad2;
    int8_t sr2;

    /* Filter registers (simplified) */
    int8_t cutoff;     /* only low byte used */
    int8_t filterCtrl; /* resonance + filter mode bits */
    int8_t volume;     /* top nibble=filter bits, lower nibble=master vol */
} sidRegs_t;

void sidChannelInit(sidChannel_t *ch);
void sidInit(sid_t *sid);
unsigned triangleSidChannel(sidChannel_t *ch);
unsigned noiseSidChannel(sidChannel_t *ch);
void clockSidChannel(sidChannel_t *ch, int cycles);
float getOutputSidChannel(sidChannel_t *ch);
int32_t bufferSamplesSid(sid_t *sid,
                         int cpuCycles,
                         const sidRegs_t *regs,
                         int16_t *outSamples,
                         int32_t maxSamples);
void sidFilterStep(float in, float cutoff, float resonance, uint8_t filterSel,
                   filterState_t *st, float *out);
