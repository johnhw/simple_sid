#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "simple_sid.h" 


/* --------------------------------------------------------------
   writeWavMono16: writes a mono 16-bit PCM .wav file
   -------------------------------------------------------------- */
static void writeWavMono16(const char *filename,
                           const int16_t *samples,
                           int numSamples,
                           int sampleRate)
{
    /* RIFF header fields */
    uint32_t dataSize   = numSamples * sizeof(int16_t);
    uint32_t fileSize   = 36 + dataSize;  /* 36 + subchunk2Size */
    uint16_t channels   = 1;
    uint16_t bitsPerSample = 16;
    uint16_t audioFormat = 1; /* PCM */
    uint32_t byteRate   = sampleRate * channels * (bitsPerSample / 8);
    uint16_t blockAlign = channels * (bitsPerSample / 8);

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Cannot open %s for writing.\n", filename);
        return;
    }

    /* Write the RIFF chunk descriptor */
    fwrite("RIFF", 1, 4, fp);
    fwrite(&fileSize, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);

    /* Write the 'fmt ' sub-chunk */
    fwrite("fmt ", 1, 4, fp);
    {
        uint32_t subchunkSize = 16; /* PCM */
        fwrite(&subchunkSize, 4, 1, fp);
        fwrite(&audioFormat, 2, 1, fp);
        fwrite(&channels, 2, 1, fp);
        fwrite(&sampleRate, 4, 1, fp);
        fwrite(&byteRate, 4, 1, fp);
        fwrite(&blockAlign, 2, 1, fp);
        fwrite(&bitsPerSample, 2, 1, fp);
    }

    /* Write the 'data' sub-chunk */
    fwrite("data", 1, 4, fp);
    fwrite(&dataSize, 4, 1, fp);

    /* Write the samples */
    fwrite(samples, sizeof(int16_t), numSamples, fp);

    fclose(fp);
}

/* --------------------------------------------------------------
   freqToSidRegister:
   A quick approximate formula from Hz to SID freq register.
   Real formula often: freq_reg = (freq_in_Hz * 16777216) / clock
   For ~985248 Hz clock, that’s ~ (freq_in_Hz * 17.04).
   We'll do a rough approximation. 
   Adjust if you want better pitch accuracy.
   -------------------------------------------------------------- */
static uint16_t freqToSidRegister(float hz)
{
    /* About 17 might be used for PAL 985248. For demonstration only: */
    return (uint16_t)(hz * 17.0f + 0.5f);
}

/* --------------------------------------------------------------
   main: demonstration harness
   -------------------------------------------------------------- */
int compex_main(void)
{
    /* 1) Create a SID instance and initialize. */
    sid_t mySid;
    sidInit(&mySid);

    /* 2) We'll generate 4 seconds of audio at 44.1kHz => 176400 samples. */
    const int sampleRate   = 44100;
    const float duration   = 4.0f;  
    const int totalSamples = (int)(sampleRate * duration);

    /* We'll store the samples in a static array. */
    int16_t *waveData = (int16_t*)calloc(totalSamples, sizeof(int16_t));
    if (!waveData) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    /* 3) We'll define a major scale for channel1 (pulse/square). 
       Let's pick a C major scale from C4 to C5. 
       Or you can pick any scale you like. 
       We have 8 notes, each lasting 0.5 seconds => 22050 samples. */
    float scaleFreqs[8] = { 261.63f, 293.66f, 329.63f, 349.23f,
                            392.00f, 440.00f, 493.88f, 523.25f };
    const int notesCount = 8;
    const int samplesPerNote = sampleRate / 2; /* 0.5 seconds each */

    /* 4) Setup initial registers. We’ll update them on the fly each sample:
         - Channel 0: we'll do the major scale with a pulse wave.
         - Channel 1: a drone in triangle wave at ~110 Hz
         - Channel 2: noise
         - Also a simple envelope AD=some quick attack, short decay, SR= no sustain or short release
    */
    sidRegs_t regs;
    memset(&regs, 0, sizeof(regs));

    /* Channel 0 => Pulse wave + gate => waveform=0x41 
         (0x40 = pulse bit, 0x01 = gate bit).
       We'll set AD = 0x11 => Attack=1, Decay=1 (just for demonstration)
       SR = 0x00 => Sustain=0, Release=0 
       Pulse width => e.g. 0x0800 => middle duty
    */
    regs.waveform0 = 0x41; /* pulse + gate */
    regs.ad0       = 0x11;
    regs.sr0       = 0xF0;
    regs.pulse0    = 0x0800;

    /* Channel 1 => Triangle wave + gate => 0x11 
         Let's do 110 Hz as a drone
    */
    regs.waveform1 = 0x11; /* triangle + gate */
    regs.ad1       = 0x22;
    regs.sr1       = 0xF0;
    regs.freq1     = freqToSidRegister(110.0f);
    regs.pulse1    = 0; /* not used for triangle */

    /* Channel 2 => Noise + gate => 0x81 
         We'll do a constant freq or 0 freq? 
         Let's pick 500 freq for variety. 
         Attack/decay set to something modest
    */
    regs.waveform2 = 0x81; 
    regs.ad2       = 0x33;
    regs.sr2       = 0xF0;
    regs.freq2     = freqToSidRegister(500.0f);

    /* 5) We'll ramp filter cutoff from 0..2047 over all 4 seconds
         Also enable a lowpass + bandpass combination for a swirl effect, 
         or you can do 0x70 => HP+BP+LP bits. For demonstration let's 
         do lowpass only => 0x10.  We'll set resonance bits in filterCtrl
         top nibble => resonance, e.g. 0x90 => resonance=9(??) => quite high
         lower nibble => other controls. 
         We'll also route all channels into filter => bit0 => ch0, bit1 => ch1, bit2 => ch2
         so filterCtrl= 0x07 => all filtered, plus resonance nibble => 0x90 => combined => 0x97?
    */
    regs.filterCtrl = 0x97;  /* (resonance=9<<4) + (ch0/ch1/ch2 => 0x07) */
    regs.volume     = 0x1f;  /* (0x1 => volume=15 => 0xf, 0x10 => lowpass bit ) 
                                Actually let's do 0x1f => 0x1*16=16 (0x10 => lowpass?), 
                                + 0xf => max volume 
                                But note we said "lowpass bit is 0x10 in the filterSel" 
                                Actually in many SID docs, 0x10 sets LP, 0x20=BP, 0x40=HP in the *same nibble*. 
                                So let's do 0x10 for LP only, plus volume=15 => 0x0f => total 0x1F 
                              */
    /* So that means filterSel=0x10 => LP, masterVol=0xf => maximum. */

    /* We'll produce 4 seconds of samples. For each sample:
       - determine which note is playing for channel0
       - set channel0 freq
       - set filter cutoff proportionally to progress
       - call bufferSamplesSid(...) with ~22 CPU cycles => yields ~1 sample
    */
    for (int i = 0; i < totalSamples; i++) {
        /* Which note of the scale are we on? */
        int noteIndex = (i / samplesPerNote);
        if (noteIndex >= notesCount) noteIndex = notesCount - 1;
        float freq    = scaleFreqs[noteIndex];
        regs.freq0    = freqToSidRegister(freq);

        /* Ramp cutoff from 0..2047 across the entire 4s */
        float frac  = (float)i / (float)(totalSamples - 1);
        int cutoff  = (int)(frac * 255.0f + 0.5f);
        if (cutoff > 255) cutoff = 255;
        regs.cutoff = (int8_t)(cutoff & 0xff); /* low 8 bits used by the example code */

        /* We want 1 sample at 44.1kHz => ~22.26 CPU cycles of PAL SID clock. 
           Let's approximate int cycles=22.
        */
        int cyclesThisSample = 22;

        /* Generate up to 1 sample. 
           Typically bufferSamplesSid can produce more, but it depends on sid->cyclesPerSample.
           We keep it in sync so it basically produces 1 sample each call. 
        */
        int n = bufferSamplesSid(&mySid, cyclesThisSample, &regs,
                                 &waveData[i], 1);
        if (n < 1) {
            /* If we somehow didn't produce a sample, just force 0. */
            waveData[i] = 0;
        }
    }

    /* 6) Write the samples to a 16-bit mono .wav file at 44.1kHz */
    writeWavMono16("sid_test.wav", waveData, totalSamples, sampleRate);

    printf("Wrote %d samples to sid_test.wav\n", totalSamples);

    free(waveData);
    return 0;
}


int simple_main(void)
{
    /* 1) Create and init the SID object */
    sid_t mySid;
    sidInit(&mySid);

    /* 2) We'll output 10s of audio at 44.1kHz => 441000 samples */
    const int sampleRate   = 44000;
    const float duration   = 10.0f; 
    const int totalSamples = (int)(sampleRate * duration);
    printf("\033[2J\n");
    int16_t *waveData = (int16_t*)calloc(totalSamples, sizeof(int16_t));
    if (!waveData) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    /* 3) Setup the registers: 
         - Channel 0: square wave (waveform=0x41 => 0x40=Pulse, 0x01=Gate)
         - freq ~ 440 Hz
         - pulse=0x0800 => ~50% duty
         - AD=0x11 => short attack/decay, SR=0x00 => no sustain, no release
         - volume=0x0f => max volume, no filter bits in top nibble => 0x00
    */
    sidRegs_t regs;
    memset(&regs, 0, sizeof(regs));

    regs.freq0    = freqToSidRegister(440.0f);
    regs.pulse0   = 0x0400;
    regs.waveform0= 0x41; /* pulse+gate */
    regs.ad0      = 0x1D;
    regs.sr0      = 0x20;

    /* No filter => top nibble=0 => filter bits=0, bottom nibble=0x0f => volume=15 */
    regs.volume   = 0x0f;

    /* 4) Fill buffer by calling bufferSamplesSid to produce one sample at a time. 
          Typically ~22 cycles per sample for PAL => we do int cycles=22 each iteration. 
    */
    int16_t *bufPtr = waveData;
    for (int i = 0; i < totalSamples; i+=400) {
        /* We'll just always pass 22 CPU cycles each time => roughly 1 sample. */
        int cyclesForOneSample = 22;  
        int generated = bufferSamplesSid(&mySid,
                                         cyclesForOneSample * 400,
                                         &regs,
                                         bufPtr, /* pointer to current sample */
                                         400);           /* space for 1 sample */
        bufPtr += generated;
    }

    /* 5) Write result to a .wav file */
    writeWavMono16("test_simple.wav", waveData, totalSamples, sampleRate);

    free(waveData);
    return 0;
}

int main(int argc, char *argv[])
{
    return simple_main();
 
}