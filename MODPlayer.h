/*
    MODPlayer.h
    Standalone ESP32 MOD player
    Decoder extracted verbatim from ESP8266Audio AudioGeneratorMOD
    by Earle F. Philhower, III (GPL licensed)
    Modified: SdFat file backend, i2s_port_t output, public UI state
*/

#pragma once

#include <SdFat.h>
#include <driver/i2s.h>
#include <stdint.h>

class MODPlayer {
public:
    // Pass the I2S port you've already configured in your sketch
    MODPlayer(i2s_port_t port = I2S_NUM_0);
    ~MODPlayer();

    // Config — call before begin()
    bool SetSampleRate(int hz)        { if (running) return false; sampleRate = hz; return true; }
    bool SetBufferSize(int sz)        { if (running) return false; fatBufferSize = sz; return true; }
    bool SetStereoSeparation(int sep) { if (running) return false; stereoSeparation = sep; return true; }
    bool SetPAL(bool use)             { if (running) return false; usePAL = use; return true; }

    // Lifecycle
    bool begin(SdFile &f);
    bool loop();
    bool stop();
    bool isRunning() { return running; }

    // ── Tracker UI state — updated every row ─────────────────────────────
    struct UIState {
        uint8_t  row;
        uint8_t  orderIndex;
        uint8_t  pattern;
        uint8_t  speed;
        uint16_t bpm;
        uint8_t  numChannels;
        struct Channel {
            uint8_t note8;
            uint8_t sampleNumber;
            uint8_t effectNumber;
            uint8_t effectParam;
            uint8_t volume;
        } ch[8];
    } ui;

protected:
    bool LoadMOD();
    bool LoadHeader();
    void GetSample(int16_t sample[2]);
    bool RunPlayer();
    void LoadSamples();
    bool LoadPattern(uint8_t pattern);
    bool ProcessTick();
    bool ProcessRow();
    void Tremolo(uint8_t channel);
    void Portamento(uint8_t channel);
    void Vibrato(uint8_t channel);

    int  mixerTick;
    enum { BITDEPTH = 16 };
    int  sampleRate;
    int  fatBufferSize;
    enum { FIXED_DIVIDER = 10 };
    int  stereoSeparation;
    bool usePAL;
    int  AMIGA;

    void UpdateAmiga() {
        AMIGA = ((usePAL ? 7159091 : 7093789) / 2 / sampleRate << FIXED_DIVIDER);
    }

    enum { ROWS = 64, SAMPLES = 31, CHANNELS = 8, NONOTE = 0xFFFF, NONOTE8 = 0xff };

    typedef struct Sample {
        uint16_t length;
        int8_t   fineTune;
        uint8_t  volume;
        uint16_t loopBegin;
        uint16_t loopLength;
    } Sample;

    typedef struct mod {
        Sample  samples[SAMPLES];
        uint8_t songLength;
        uint8_t numberOfPatterns;
        uint8_t order[128];
        uint8_t numberOfChannels;
    } mod;

    typedef struct Pattern {
        uint8_t sampleNumber[ROWS][CHANNELS];
        uint8_t note8[ROWS][CHANNELS];
        uint8_t effectNumber[ROWS][CHANNELS];
        uint8_t effectParameter[ROWS][CHANNELS];
    } Pattern;

    typedef struct player {
        Pattern  currentPattern;
        uint32_t amiga;
        uint16_t samplesPerTick;
        uint8_t  speed;
        uint8_t  tick;
        uint8_t  row;
        uint8_t  lastRow;
        uint8_t  orderIndex;
        uint8_t  oldOrderIndex;
        uint8_t  patternDelay;
        uint8_t  patternLoopCount[CHANNELS];
        uint8_t  patternLoopRow[CHANNELS];
        uint8_t  lastSampleNumber[CHANNELS];
        int8_t   volume[CHANNELS];
        uint16_t lastNote[CHANNELS];
        uint16_t amigaPeriod[CHANNELS];
        int16_t  lastAmigaPeriod[CHANNELS];
        uint16_t portamentoNote[CHANNELS];
        uint8_t  portamentoSpeed[CHANNELS];
        uint8_t  waveControl[CHANNELS];
        uint8_t  vibratoSpeed[CHANNELS];
        uint8_t  vibratoDepth[CHANNELS];
        int8_t   vibratoPos[CHANNELS];
        uint8_t  tremoloSpeed[CHANNELS];
        uint8_t  tremoloDepth[CHANNELS];
        int8_t   tremoloPos[CHANNELS];
    } player;

    typedef struct mixer {
        uint32_t sampleBegin[SAMPLES];
        uint32_t sampleEnd[SAMPLES];
        uint32_t sampleloopBegin[SAMPLES];
        uint16_t sampleLoopLength[SAMPLES];
        uint32_t sampleLoopEnd[SAMPLES];
        uint8_t  channelSampleNumber[CHANNELS];
        uint32_t channelSampleOffset[CHANNELS];
        uint16_t channelFrequency[CHANNELS];
        uint8_t  channelVolume[CHANNELS];
        uint8_t  channelPanning[CHANNELS];
    } mixer;

    typedef struct fatBuffer {
        uint8_t  *channels[CHANNELS];
        uint32_t  samplePointer[CHANNELS];
        uint8_t   channelSampleNumber[CHANNELS];
    } fatBuffer;

    typedef enum {
        ARPEGGIO = 0, PORTAMENTOUP, PORTAMENTODOWN, TONEPORTAMENTO,
        VIBRATO, PORTAMENTOVOLUMESLIDE, VIBRATOVOLUMESLIDE, TREMOLO,
        SETCHANNELPANNING, SETSAMPLEOFFSET, VOLUMESLIDE, JUMPTOORDER,
        SETVOLUME, BREAKPATTERNTOROW, ESUBSET, SETSPEED
    } EffectsValues;

    typedef enum {
        SETFILTER = 0, FINEPORTAMENTOUP, FINEPORTAMENTODOWN, GLISSANDOCONTROL,
        SETVIBRATOWAVEFORM, SETFINETUNE, PATTERNLOOP, SETTREMOLOWAVEFORM,
        SUBEFFECT8, RETRIGGERNOTE, FINEVOLUMESLIDEUP, FINEVOLUMESLIDEDOWN,
        NOTECUT, NOTEDELAY, PATTERNDELAY, INVERTLOOP
    } Effect08Subvalues;

public:
    player    Player;
    mod       Mod;
    mixer     Mixer;
    fatBuffer FatBuffer;
    int16_t waveRing[182];
    uint8_t wavePos;
    float gain = 1.0f;

private:
    bool        running;
    SdFile     *file;
    i2s_port_t  i2sPort;
    int16_t     i2sBuf[512 * 2];
    int         i2sBufPos;
    int16_t     lastSample[2];

    void flushI2S();
    void updateUI();
};
