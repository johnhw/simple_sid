#include "simple_sid.h"

/* ------------------------------------------------------------------
   Internal tables for ADSR increments & sustain levels
   ------------------------------------------------------------------ */
static unsigned short adsrRateTable[] = {
    9, 32, 63, 95, 149, 220, 267, 313,
    392, 977, 1954, 3126, 3907, 11720, 19532, 31251};

static uint8_t sustainLevels[] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

static uint8_t expTargetTable[] = {
    1, 30, 30, 30, 30, 30, 16, 16, 16, 16, 16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

/* ------------------------------------------------------------------
   Channel init
   ------------------------------------------------------------------ */
void sidChannelInit(sidChannel_t *ch)
{
    ch->frequency = 0;
    ch->ad = 0;
    ch->sr = 0;
    ch->pulse = 0;
    ch->waveform = 0;
    ch->doSync = false;
    ch->state = RELEASE;
    ch->accumulator = 0;
    ch->noiseGenerator = 0x7ffff8; /* Some initial seed */
    ch->adsrCounter = 0;
    ch->adsrExpCounter = 0;
    ch->volumeLevel = 0;
    ch->syncTarget = NULL;
    ch->syncSource = NULL;
}

/* ------------------------------------------------------------------
   SID initialization
   (PAL ~ 63*312*50 = ~982,800 cycles/sec, 44.1kHz => ~22.3 cyc/sample)
   ------------------------------------------------------------------ */
void sidInit(sid_t *sid)
{
    int i;
    sid->cyclesPerSample = (63.f * 312.f * 50.f) / 44100.f;
    sid->cycleAccumulator = 0.f;
    sid->filter.low = 0.f;
    sid->filter.band = 0.f;

    for (i = 0; i < 3; i++)
        sidChannelInit(&sid->channels[i]);

    /* Wire up channel sync pointers */
    sid->channels[0].syncTarget = &sid->channels[1];
    sid->channels[1].syncTarget = &sid->channels[2];
    sid->channels[2].syncTarget = &sid->channels[0];

    sid->channels[0].syncSource = &sid->channels[2];
    sid->channels[1].syncSource = &sid->channels[0];
    sid->channels[2].syncSource = &sid->channels[1];
}

/* ------------------------------------------------------------------
   (longer) helper: triangle
   ------------------------------------------------------------------ */
unsigned triangleSidChannel(sidChannel_t *ch)
{
    unsigned t = ch->accumulator;
    if (ch->waveform & 0x04) /* ringmod bit? */
        t ^= ch->syncSource->accumulator;

    if (t >= 0x800000)
        t = (ch->accumulator ^ 0xffffff);
    return (t >> 7) & 0xffff;
}

/* ------------------------------------------------------------------
   (longer) helper: noise
   ------------------------------------------------------------------ */
unsigned noiseSidChannel(sidChannel_t *ch)
{
    unsigned tmp = 0;
    tmp += (ch->noiseGenerator & 0x100000) >> 5;
    tmp += (ch->noiseGenerator & 0x40000) >> 4;
    tmp += (ch->noiseGenerator & 0x4000) >> 1;
    tmp += (ch->noiseGenerator & 0x800) << 1;
    tmp += (ch->noiseGenerator & 0x200) << 2;
    tmp += (ch->noiseGenerator & 0x20) << 5;
    tmp += (ch->noiseGenerator & 0x04) << 7;
    tmp += (ch->noiseGenerator & 0x01) << 8;
    return tmp;
}

/* ------------------------------------------------------------------
   Clock a channel's accumulator + ADSR for 'cycles'
   ------------------------------------------------------------------ */
void clockSidChannel(sidChannel_t *ch, int cycles)
{
    /* Gate bit => Attack; else Release */
    if (ch->waveform & 0x01)
    {
        if (ch->state == RELEASE)
            ch->state = ATTACK;
    }
    else
    {
        ch->state = RELEASE;
    }

    /* --- ADSR update --- */
    {
        int adsrCycles = cycles;
        while (adsrCycles > 0)
        {
            unsigned short rate;
            switch (ch->state)
            {
            case ATTACK:
                rate = adsrRateTable[ch->ad >> 4];
                break;
            case DECAY:
                rate = adsrRateTable[ch->ad & 0x0f];
                break;
            default: /* RELEASE */
                rate = adsrRateTable[ch->sr & 0x0f];
                break;
            }

            /* how many cycles until adsrCounter == rate ? */
            int needed = (ch->adsrCounter < rate)
                             ? (rate - ch->adsrCounter)
                             : (0x8000 + rate - ch->adsrCounter);
            int stepNow = (adsrCycles < needed) ? adsrCycles : needed;

            ch->adsrCounter = (ch->adsrCounter + stepNow) & 0x7fff;

            if (ch->adsrCounter == rate)
            {
                ch->adsrCounter = 0;
                switch (ch->state)
                {
                case ATTACK:
                    ch->adsrExpCounter = 0;
                    ch->volumeLevel++;
                    if (ch->volumeLevel == 0xff)
                        ch->state = DECAY;
                    break;
                case DECAY:
                {
                    uint8_t expTarget = (ch->volumeLevel < 0x5d)
                                            ? expTargetTable[ch->volumeLevel]
                                            : 1;
                    ch->adsrExpCounter++;
                    if (ch->adsrExpCounter >= expTarget)
                    {
                        ch->adsrExpCounter = 0;
                        if (ch->volumeLevel > sustainLevels[ch->sr >> 4])
                            ch->volumeLevel--;
                    }
                }
                break;
                case RELEASE:
                {
                    if (ch->volumeLevel > 0)
                    {
                        uint8_t expTarget = (ch->volumeLevel < 0x5d)
                                                ? expTargetTable[ch->volumeLevel]
                                                : 1;
                        ch->adsrExpCounter++;
                        if (ch->adsrExpCounter >= expTarget)
                        {
                            ch->adsrExpCounter = 0;
                            ch->volumeLevel--;
                        }
                    }
                }
                break;
                }
            }
            adsrCycles -= stepNow;
        }
    }

    /* Test bit => zero accumulator */
    if (ch->waveform & 0x08)
    {
        ch->accumulator = 0;
        return;
    }

    /* Frequency=0 => no progress */
    if (ch->frequency == 0)
        return;

    /* If no noise (0x80) and syncTarget has no sync bit (0x02), do fast update */
    if (((ch->waveform & 0x80) == 0) && ((ch->syncTarget->waveform & 0x02) == 0))
    {
        unsigned inc = ch->frequency * (unsigned)cycles;
        ch->accumulator = (ch->accumulator + inc) & 0xffffff;
        return;
    }

    /* Otherwise, step carefully for noise or sync triggers */
    {
        int left = cycles;
        while (left > 0)
        {
            int stepNow = left;
            unsigned lastAcc = ch->accumulator;

            /* Noise bit crossing? (bit19 => 0x80000) */
            if (ch->waveform & 0x80)
            {
                unsigned mask20 = (ch->accumulator & 0xfffff);
                if (mask20 < 0x80000)
                {
                    int needed = (int)((0x80000 - mask20) / ch->frequency) + 1;
                    if (needed < stepNow)
                        stepNow = needed;
                }
                else
                {
                    int needed = (int)((0x180000 - mask20) / ch->frequency) + 1;
                    if (needed < stepNow)
                        stepNow = needed;
                }
            }

            /* Sync detection? (bit23 => 0x800000) */
            if (ch->syncTarget->waveform & 0x02)
            {
                if (ch->accumulator < 0x800000)
                {
                    int needed = (int)((0x800000 - ch->accumulator) / ch->frequency) + 1;
                    if (needed < stepNow)
                        stepNow = needed;
                }
                else
                {
                    int needed = (int)((0x1800000 - ch->accumulator) / ch->frequency) + 1;
                    if (needed < stepNow)
                        stepNow = needed;
                }
            }

            ch->accumulator = (ch->accumulator +
                               (ch->frequency * (unsigned)stepNow)) &
                              0xffffff;

            /* If noise bit crosses 0->1 in bit19, update LFSR */
            if (ch->waveform & 0x80)
            {
                bool wasLow = ((lastAcc & 0x80000) == 0);
                bool nowHigh = ((ch->accumulator & 0x80000) != 0);
                if (wasLow && nowHigh)
                {
                    unsigned tmp = ch->noiseGenerator;
                    unsigned step = (tmp & 0x400000) ^
                                    ((tmp & 0x20000) << 5);
                    tmp <<= 1;
                    if (step)
                        tmp |= 1;
                    ch->noiseGenerator = tmp & 0x7fffff;
                }
            }

            /* 0->1 crossing in bit23 => sync */
            ch->doSync = false;
            {
                bool wasLow = ((lastAcc & 0x800000) == 0);
                bool nowHigh = ((ch->accumulator & 0x800000) != 0);
                if (wasLow && nowHigh)
                    ch->doSync = true;
            }

            left -= stepNow;
        }
    }
}

/* ------------------------------------------------------------------
   Get channel output as float [-1..+1] scaled by envelope
   ------------------------------------------------------------------ */
float getOutputSidChannel(sidChannel_t *ch)
{    
    if (ch->volumeLevel == 0)
        return 0.f;

    unsigned waveOut = 0;

    switch (ch->waveform & 0xf0)
    {
    case 0x10: /* Triangle */
        waveOut = triangleSidChannel(ch);
        break;

    case 0x20: /* Sawtooth */
        /* was: sawtoothSidChannel(ch) => (ch->accumulator >> 8) */
        waveOut = (ch->accumulator >> 8);
        break;

    case 0x40: /* Pulse */
        /* was: pulseSidChannel(ch) => top12 = ch->accumulator >> 12 ... */
        {
            unsigned top12 = ch->accumulator >> 12;
            waveOut = (top12 >= (ch->pulse & 0x0fff)) ? 0xffff : 0x0000;
        }
        break;

    case 0x50: /* Tri + Pulse */
    {
        unsigned tri = triangleSidChannel(ch);
        unsigned sq;
        {
            unsigned top12 = ch->accumulator >> 12;
            sq = (top12 >= (ch->pulse & 0x0fff)) ? 0xffff : 0x0000;
        }
        unsigned combo = (sq & tri & (tri >> 1)) & (tri << 1);
        waveOut = combo << 1;
        if (waveOut > 0xffff)
            waveOut = 0xffff;
    }
    break;

    case 0x60: /* Saw + Pulse */
    {
        unsigned saw = (ch->accumulator >> 8);
        unsigned sq;
        {
            unsigned top12 = ch->accumulator >> 12;
            sq = (top12 >= (ch->pulse & 0x0fff)) ? 0xffff : 0x0000;
        }
        unsigned combo = (sq & saw & (saw >> 1)) & (saw << 1);
        waveOut = combo << 1;
        if (waveOut > 0xffff)
            waveOut = 0xffff;
    }
    break;

    case 0x70: /* Tri + Saw + Pulse */
    {
        unsigned triSaw = triangleSidChannel(ch) & (ch->accumulator >> 8);
        unsigned sq;
        {
            unsigned top12 = ch->accumulator >> 12;
            sq = (top12 >= (ch->pulse & 0x0fff)) ? 0xffff : 0x0000;
        }
        unsigned combo = (sq & triSaw & (triSaw >> 1)) & (triSaw << 1);
        waveOut = combo << 1;
        if (waveOut > 0xffff)
            waveOut = 0xffff;
    }
    break;

    case 0x80: /* Noise */
        waveOut = noiseSidChannel(ch);
        break;

    default:
        break;
    }

    /* center at 0x8000 => signed -32768..+32767 */
    int centered = (int)waveOut - 0x8000;
    float env = (ch->volumeLevel / 255.0f);    
    /* scale to [-1..+1] or so */
    return (centered * env) / 32768.0f;
}


void dumpSID(int cpuCycles, int32_t maxSamples, const sidRegs_t *regs, sid_t *sid)
{
    // Clear screen
    printf("\033[2J\n");

    // Global settings box
    printf("\033[2;5H\033[1;37m┌────────────────────────────────────────────────────────────────────────────┐\033[0m");
    printf("\033[3;5H\033[1;37m│\033[0m cpuCycles:  \033[1;32m%05d\033[0m maxSamples: \033[1;32m%05d\033[0m \033[1;37m                                       │\033[0m", cpuCycles, maxSamples);
    printf("\033[4;5H\033[1;37m│\033[0m freq0:      \033[1;32m%05d\033[0m pulse0:     \033[1;32m%05d\033[0m waveform0: \033[1;32m%05d\033[0m ad0: \033[1;32m%05d\033[0m sr0: \033[1;32m%05d\033[0m \033[1;37m│\033[0m", regs->freq0, regs->pulse0, regs->waveform0, regs->ad0, regs->sr0);
    printf("\033[5;5H\033[1;37m│\033[0m freq1:      \033[1;32m%05d\033[0m pulse1:     \033[1;32m%05d\033[0m waveform1: \033[1;32m%05d\033[0m ad1: \033[1;32m%05d\033[0m sr1: \033[1;32m%05d\033[0m \033[1;37m│\033[0m", regs->freq1, regs->pulse1, regs->waveform1, regs->ad1, regs->sr1);
    printf("\033[6;5H\033[1;37m│\033[0m freq2:      \033[1;32m%05d\033[0m pulse2:     \033[1;32m%05d\033[0m waveform2: \033[1;32m%05d\033[0m ad2: \033[1;32m%05d\033[0m sr2: \033[1;32m%05d\033[0m \033[1;37m│\033[0m", regs->freq2, regs->pulse2, regs->waveform2, regs->ad2, regs->sr2);
    printf("\033[7;5H\033[1;37m│\033[0m cutoff:     \033[1;32m%05d\033[0m filterCtrl: \033[1;32m%05d\033[0m volume:    \033[1;32m%05d\033[0m \033[1;37m                      │\033[0m", regs->cutoff, regs->filterCtrl, regs->volume);
    printf("\033[8;5H\033[1;37m│\033[0m cyclesSam:  \033[1;32m%5.4f\033[0m cycleAccumulator: \033[1;32m%5.4f\033[0m \033[1;37m                             │\033[0m", sid->cyclesPerSample, sid->cycleAccumulator);
    printf("\033[9;5H\033[1;37m│\033[0m filter.low: \033[1;32m%5.4f\033[0m filter.band:      \033[1;32m%5.4f\033[0m \033[1;37m                               │\033[0m", sid->filter.low, sid->filter.band);
    printf("\033[10;5H\033[1;37m└────────────────────────────────────────────────────────────────────────────┘\033[0m");

    // Channel settings boxes
    for (int i = 0; i < 3; i++)
    {
        int baseRow = 11 + i * 6;
        const char *color = (i == 0) ? "\033[1;34m" : (i == 1) ? "\033[1;33m" : "\033[1;31m";

        printf("\033[%d;5H%s┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\033[0m", baseRow, color);
        printf("\033[%d;5H%s│\033[0m channels[%d].frequency:   \033[1;32m%05d\033[0m pulse:          \033[1;32m%05d\033[0m waveform:    \033[1;32m%05d\033[0m ad:             \033[1;32m%05d\033[0m sr: \033[1;32m%05d\033[0m %s   │\033[0m", baseRow + 1, color, i, sid->channels[i].frequency, sid->channels[i].pulse, sid->channels[i].waveform, sid->channels[i].ad, sid->channels[i].sr, color);
        printf("\033[%d;5H%s│\033[0m channels[%d].doSync:      \033[1;32m%05d\033[0m state:          \033[1;32m%05d\033[0m accumulator: \033[1;32m%08d\033[0m noiseGenerator: \033[1;32m%05d\033[0m %s        │\033[0m", baseRow + 2, color, i, sid->channels[i].doSync, sid->channels[i].state, sid->channels[i].accumulator, sid->channels[i].noiseGenerator, color);
        printf("\033[%d;5H%s│\033[0m channels[%d].adsrCounter: \033[1;32m%05d\033[0m adsrExpCounter: \033[1;32m%05d\033[0m volumeLevel: \033[1;32m%05d\033[0m %s                                   │\033[0m", baseRow + 3, color, i, sid->channels[i].adsrCounter, sid->channels[i].adsrExpCounter, sid->channels[i].volumeLevel, color);
        printf("\033[%d;5H%s│\033[0m channels[%d].syncTarget:  \033[1;32m%p\033[0m        syncSource:       \033[1;32m%p\033[0m %s                           │\033[0m", baseRow + 4, color, i, sid->channels[i].syncTarget, sid->channels[i].syncSource, color);
        printf("\033[%d;5H%s└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘\033[0m", baseRow + 5, color);
    }
}
/* ------------------------------------------------------------------
   Advance SID by cpuCycles, produce audio samples in outSamples
   Returns number of samples written (up to maxSamples).
   ------------------------------------------------------------------ */
int32_t bufferSamplesSid(sid_t *sid,
                         int cpuCycles,
                         const sidRegs_t *regs,
                         int16_t *outSamples,
                         int32_t maxSamples)
{
    int32_t outIndex = 0;
    if (cpuCycles <= 0 || maxSamples <= 0)
        return 0;
    
    //dumpSID(cpuCycles, maxSamples, regs, sid);
    
    /* 1) Update channel register values from sidRegs_t */
    sid->channels[0].frequency = (uint16_t)regs->freq0;
    sid->channels[0].pulse = (uint16_t)regs->pulse0;
    sid->channels[0].waveform = (uint8_t)regs->waveform0;
    sid->channels[0].ad = (uint8_t)regs->ad0;
    sid->channels[0].sr = (uint8_t)regs->sr0;

    sid->channels[1].frequency = (uint16_t)regs->freq1;
    sid->channels[1].pulse = (uint16_t)regs->pulse1;
    sid->channels[1].waveform = (uint8_t)regs->waveform1;
    sid->channels[1].ad = (uint8_t)regs->ad1;
    sid->channels[1].sr = (uint8_t)regs->sr1;

    sid->channels[2].frequency = (uint16_t)regs->freq2;
    sid->channels[2].pulse = (uint16_t)regs->pulse2;
    sid->channels[2].waveform = (uint8_t)regs->waveform2;
    sid->channels[2].ad = (uint8_t)regs->ad2;
    sid->channels[2].sr = (uint8_t)regs->sr2;

    /* The volume register also encodes filter bits (0x70) + vol in lower nibble */
    float masterVol = (float)((regs->volume) & 0x0f) / 22.5f;
    uint8_t filterSel = ((uint8_t)regs->volume) & 0x70; /* bits 4..6 */
    uint8_t filterCtrl = (uint8_t)regs->filterCtrl;

    /* The code uses only the low byte of cutoff (regs->cutoff). */
    float cutoff = 0.05f + 0.85f * (sinf(((float)regs->cutoff / 255.f - 0.5f) * (float)M_PI) * 0.5f + 0.5f);
    cutoff = powf(cutoff, 1.3f);

    /* Resonance from upper nibble of filterCtrl if >0x3f, else default. */
    float resonance = 1.75f;
    if (filterCtrl > 0x3f)
    {
        uint8_t r = (filterCtrl >> 4);
        if (r > 0)
            resonance = 7.f / (float)r;
    }

    /* 2) Step through CPU cycles, generate samples after enough accumulates. */
    while (cpuCycles > 0 && outIndex < maxSamples)
    {
        /* how many cycles until next sample? */
        float needed = sid->cyclesPerSample - sid->cycleAccumulator;
        if (needed < 0.f)
            needed = 0.f;
        int stepNow = (cpuCycles < (int)ceilf(needed)) ? cpuCycles : (int)ceilf(needed);

        /* Clock each channel */
        clockSidChannel(&sid->channels[0], stepNow);
        clockSidChannel(&sid->channels[1], stepNow);
        clockSidChannel(&sid->channels[2], stepNow);

        /* Apply sync if doSync is set and target has sync-bit (0x2) */
        {
            for (int i = 0; i < 3; i++)
            {
                if (sid->channels[i].doSync &&
                    (sid->channels[i].syncTarget->waveform & 0x02))
                {
                    /* resetAccumulatorSidChannel(...) => ch->accumulator = 0; */
                    sid->channels[i].syncTarget->accumulator = 0;
                }
            }
        }

        sid->cycleAccumulator += stepNow;
        if (sid->cycleAccumulator >= sid->cyclesPerSample)
        {
            sid->cycleAccumulator -= sid->cyclesPerSample;

            /* 3) Mix channels with filter routing. */
            float out = 0.f;
            float fin = 0.f;

            /* channel0 -> filter or direct? */
            {
                float c0 = getOutputSidChannel(&sid->channels[0]);
                if (filterCtrl & 0x01)
                    fin += c0;
                else
                    out += c0;
            }
            /* channel1 */
            {
                float c1 = getOutputSidChannel(&sid->channels[1]);
                if (filterCtrl & 0x02)
                    fin += c1;
                else
                    out += c1;
            }
            /* channel2 */
            {
                float c2 = getOutputSidChannel(&sid->channels[2]);
                if (filterCtrl & 0x04)
                    fin += c2;
                else
                    out += c2;
            }

            /* Filter the mixed channels */
            float filtered;
            sidFilterStep(fin, cutoff, resonance, filterSel, &sid->filter, &filtered);
            out += filtered;

            /* Scale by master vol, clamp, store */
            out *= masterVol;
            if (out < -1.f)
                out = -1.f;
            if (out > 1.f)
                out = 1.f;

            outSamples[outIndex++] = (int16_t)(out * 32767.f);
        }

        cpuCycles -= stepNow;
    }

    return outIndex; /* number of samples produced */
}

static float saturate(float x)
{
    /* Simple polynomial approximation to tanh or shape function. */
    /* e.g. x - x^3/6 => slight softening near +/-1 */
    const float alpha = 0.1666667f; /* 1/6 */
    return x - (x * x * x) * alpha;
}

/*
   sidFilterStep: A simple 2-pole resonant state-variable filter.
   - in         : input signal (e.g. sum of channels that go through the filter).
   - cutoff     : normalized cutoff (0..1).
   - resonance  : feedback factor; higher => more resonance. Typically 0..4 range.
   - filterSel  : bits to enable LP, BP, HP (0x10=LP, 0x20=BP, 0x40=HP).
   - st         : pointer to filter state (low, band).
   - out        : result is written here (sum of whichever modes are enabled).
*/
void sidFilterStep(float in, float cutoff, float resonance, uint8_t filterSel,
                   filterState_t *st, float *out)
{
    /* 1) Subtract some of the bandpass signal for resonance feedback. */
    float input = in - (resonance * st->band);

    /* 2) Integrator #1 => "low" output. */
    st->low += saturate(cutoff * st->band);

    /* 3) Integrator #2 => "band" output. */
    st->band += saturate(cutoff * (input - st->low));

    /* 4) The highpass output is what's "left over": input - (low + band). */
    /*   (In some variations, you might do input - low - Q*band, etc.) */
    float high = input - st->low - st->band;

    /* 5) Combine whichever modes are requested:
          bit 0x10 => Lowpass
          bit 0x20 => Bandpass
          bit 0x40 => Highpass
       This part is up to you; you can accumulate them any way you like.
    */
    float mix = 0.f;
    if (filterSel & 0x10) /* Lowpass bit */
        mix += st->low;
    if (filterSel & 0x20) /* Bandpass bit */
        mix += st->band;
    if (filterSel & 0x40) /* Highpass bit */
        mix += high;

    /* 6) Write the mixed result */
    *out = mix;
}

/* ------------------------------------------------------------------
   Example usage:

   sid_t mySid;
   sidInit(&mySid);

   sidRegs_t regs = {
       .freq0 = 440, .pulse0 = 2048, .waveform0=0x11, ...
       ...
   };

   int16_t buffer[1024];
   int written = bufferSamplesSid(&mySid, 1000, &regs, buffer, 1024);

   // 'written' is how many samples were produced.
   ------------------------------------------------------------------ */
