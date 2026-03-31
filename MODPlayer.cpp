/*
    MODPlayer.cpp
    Standalone ESP32 MOD player
    Decoder extracted verbatim from ESP8266Audio AudioGeneratorMOD
    by Earle F. Philhower, III (GPL licensed)
    Modified: SdFat file backend, i2s_port_t output, public UI state
*/

#pragma GCC optimize ("O3")

#include "MODPlayer.h"
#include <string.h>
#include <stdlib.h>

#define NOTE(r, c) (Player.currentPattern.note8[r][c]==NONOTE8?NONOTE:8*Player.currentPattern.note8[r][c])

#ifndef min
#define min(X,Y) ((X) < (Y) ? (X) : (Y))
#endif

// ── Verbatim from AudioGeneratorMOD ──────────────────────────────────────────

static const uint16_t amigaPeriods[296] PROGMEM = {
    907, 900, 894, 887, 881, 875, 868, 862,
    856, 850, 844, 838, 832, 826, 820, 814,
    808, 802, 796, 791, 785, 779, 774, 768,
    762, 757, 752, 746, 741, 736, 730, 725,
    720, 715, 709, 704, 699, 694, 689, 684,
    678, 675, 670, 665, 660, 655, 651, 646,
    640, 636, 632, 628, 623, 619, 614, 610,
    604, 601, 597, 592, 588, 584, 580, 575,
    570, 567, 563, 559, 555, 551, 547, 543,
    538, 535, 532, 528, 524, 520, 516, 513,
    508, 505, 502, 498, 494, 491, 487, 484,
    480, 477, 474, 470, 467, 463, 460, 457,
    453, 450, 447, 444, 441, 437, 434, 431,
    428, 425, 422, 419, 416, 413, 410, 407,
    404, 401, 398, 395, 392, 390, 387, 384,
    381, 379, 376, 373, 370, 368, 365, 363,
    360, 357, 355, 352, 350, 347, 345, 342,
    339, 337, 335, 332, 330, 328, 325, 323,
    320, 318, 316, 314, 312, 309, 307, 305,
    302, 300, 298, 296, 294, 292, 290, 288,
    285, 284, 282, 280, 278, 276, 274, 272,
    269, 268, 266, 264, 262, 260, 258, 256,
    254, 253, 251, 249, 247, 245, 244, 242,
    240, 238, 237, 235, 233, 232, 230, 228,
    226, 225, 223, 222, 220, 219, 217, 216,
    214, 212, 211, 209, 208, 206, 205, 203,
    202, 200, 199, 198, 196, 195, 193, 192,
    190, 189, 188, 187, 185, 184, 183, 181,
    180, 179, 177, 176, 175, 174, 172, 171,
    170, 169, 167, 166, 165, 164, 163, 161,
    160, 159, 158, 157, 156, 155, 154, 152,
    151, 150, 149, 148, 147, 146, 145, 144,
    143, 142, 141, 140, 139, 138, 137, 136,
    135, 134, 133, 132, 131, 130, 129, 128,
    127, 126, 125, 125, 123, 123, 122, 121,
    120, 119, 118, 118, 117, 116, 115, 114,
    113, 113, 112, 111, 110, 109, 109, 108
};
#define ReadAmigaPeriods(a) (uint16_t)pgm_read_word(amigaPeriods + (a))

static const uint8_t sine[64] PROGMEM = {
    0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24
};
#define ReadSine(a) pgm_read_byte(sine + (a))

static inline uint16_t MakeWord(uint8_t h, uint8_t l) {
    return h << 8 | l;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

MODPlayer::MODPlayer(i2s_port_t port) {
    i2sPort       = port;
    sampleRate    = 44100;
    fatBufferSize = 6 * 1024;
    stereoSeparation = 32;
    mixerTick     = 0;
    usePAL        = false;
    UpdateAmiga();
    running       = false;
    file          = nullptr;
    i2sBufPos     = 0;
    memset(waveRing, 0, sizeof(waveRing));
    wavePos = 0;
    memset(&ui, 0, sizeof(ui));
    memset(lastSample, 0, sizeof(lastSample));
    for (int i = 0; i < CHANNELS; i++)
        FatBuffer.channels[i] = nullptr;
}

MODPlayer::~MODPlayer() {
    for (int i = 0; i < CHANNELS; i++)
        FatBuffer.channels[i] = nullptr;
}

// ── I2S output ────────────────────────────────────────────────────────────────

void MODPlayer::flushI2S() {
    if (i2sBufPos == 0) return;
    size_t written;
    for (int i = 0; i < i2sBufPos * 2; i++) {
    int32_t s = (int32_t)i2sBuf[i] * gain;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    i2sBuf[i] = (int16_t)s;
    }
    i2s_write(i2sPort, i2sBuf, i2sBufPos * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    i2sBufPos = 0;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

bool MODPlayer::begin(SdFile &f) {
    if (running) stop();
    file = &f;

    UpdateAmiga();

    for (int i = 0; i < CHANNELS; i++) {
        FatBuffer.channels[i] = reinterpret_cast<uint8_t*>(calloc(fatBufferSize, 1));
        if (!FatBuffer.channels[i]) {
            stop();
            return false;
        }
    }

    if (!LoadMOD()) {
        stop();
        return false;
    }

    i2sBufPos = 0;
    running = true;
    return true;
}

bool MODPlayer::stop() {
    for (int i = 0; i < CHANNELS; i++) {
        free(FatBuffer.channels[i]);
        FatBuffer.channels[i] = nullptr;
    }
    flushI2S();
    running = false;
    return true;
}

bool MODPlayer::loop() {
    if (!running) return false;

    for (int n = 0; n < 512; n++) {
        if (mixerTick == 0) {
            running = RunPlayer();
            if (!running) {
                stop();
                return false;
            }
            mixerTick = Player.samplesPerTick;
        }
        GetSample(lastSample);
        mixerTick--;

        i2sBuf[i2sBufPos * 2]     = lastSample[0];
        i2sBuf[i2sBufPos * 2 + 1] = lastSample[1];
        i2sBufPos++;

        if (i2sBufPos >= 512) flushI2S();
    }
    return true;
}

// ── UI state ──────────────────────────────────────────────────────────────────

void MODPlayer::updateUI() {
    ui.row         = Player.lastRow;
    ui.orderIndex  = Player.orderIndex;
    ui.pattern     = Mod.order[Player.orderIndex];
    ui.speed       = Player.speed;
    ui.bpm         = (sampleRate * 5) / (2 * Player.samplesPerTick);
    ui.numChannels = Mod.numberOfChannels;
    for (int ch = 0; ch < Mod.numberOfChannels && ch < 8; ch++) {
        ui.ch[ch].note8        = Player.currentPattern.note8[Player.lastRow][ch];
        ui.ch[ch].sampleNumber = Player.currentPattern.sampleNumber[Player.lastRow][ch];
        ui.ch[ch].effectNumber = Player.currentPattern.effectNumber[Player.lastRow][ch];
        ui.ch[ch].effectParam  = Player.currentPattern.effectParameter[Player.lastRow][ch];
        ui.ch[ch].volume       = Mixer.channelVolume[ch];
    }
}

// ── LoadHeader — verbatim, file->read() replaced with SdFat ──────────────────

bool MODPlayer::LoadHeader() {
    uint8_t i;
    uint8_t temp[4];
    uint8_t junk[22];

    if (!file->seekSet(0)) return false;

    if (20 != file->read(junk, 20)) return false;

    for (i = 0; i < SAMPLES; i++) {
        if (22 != file->read(junk, 22)) return false;
        if (2 != file->read(temp, 2)) return false;
        Mod.samples[i].length = MakeWord(temp[0], temp[1]) * 2;
        if (1 != file->read(reinterpret_cast<uint8_t*>(&Mod.samples[i].fineTune), 1)) return false;
        if (Mod.samples[i].fineTune > 7) Mod.samples[i].fineTune -= 16;
        if (1 != file->read(&Mod.samples[i].volume, 1)) return false;
        if (2 != file->read(temp, 2)) return false;
        Mod.samples[i].loopBegin = MakeWord(temp[0], temp[1]) * 2;
        if (2 != file->read(temp, 2)) return false;
        Mod.samples[i].loopLength = MakeWord(temp[0], temp[1]) * 2;
        if (Mod.samples[i].loopBegin + Mod.samples[i].loopLength > Mod.samples[i].length) {
            Mod.samples[i].loopLength = Mod.samples[i].length - Mod.samples[i].loopBegin;
        }
    }

    if (1 != file->read(&Mod.songLength, 1)) return false;
    if (1 != file->read(temp, 1)) return false;

    Mod.numberOfPatterns = 0;
    for (i = 0; i < 128; i++) {
        if (1 != file->read(&Mod.order[i], 1)) return false;
        if (Mod.order[i] > Mod.numberOfPatterns) Mod.numberOfPatterns = Mod.order[i];
    }
    Mod.numberOfPatterns++;

    if (4 != file->read(temp, 4)) return false;
    if (!strncmp(reinterpret_cast<const char*>(temp + 1), "CHN", 3)) {
        Mod.numberOfChannels = temp[0] - '0';
    } else if (!strncmp(reinterpret_cast<const char*>(temp + 2), "CH", 2)) {
        Mod.numberOfChannels = (temp[0] - '0') * 10 + temp[1] - '0';
    } else {
        Mod.numberOfChannels = 4;
    }

    if (Mod.numberOfChannels > CHANNELS) return false;
    return true;
}

// ── LoadSamples — verbatim ────────────────────────────────────────────────────

void MODPlayer::LoadSamples() {
    uint8_t i;
    uint32_t fileOffset = 1084 + Mod.numberOfPatterns * ROWS * Mod.numberOfChannels * 4 - 1;

    for (i = 0; i < SAMPLES; i++) {
        if (Mod.samples[i].length) {
            Mixer.sampleBegin[i] = fileOffset;
            Mixer.sampleEnd[i]   = fileOffset + Mod.samples[i].length;
            if (Mod.samples[i].loopLength > 2) {
                Mixer.sampleloopBegin[i]  = fileOffset + Mod.samples[i].loopBegin;
                Mixer.sampleLoopLength[i] = Mod.samples[i].loopLength;
                Mixer.sampleLoopEnd[i]    = Mixer.sampleloopBegin[i] + Mixer.sampleLoopLength[i];
            } else {
                Mixer.sampleloopBegin[i]  = 0;
                Mixer.sampleLoopLength[i] = 0;
                Mixer.sampleLoopEnd[i]    = 0;
            }
            fileOffset += Mod.samples[i].length;
        }
    }
}

// ── LoadPattern — verbatim, seek replaced with SdFat ─────────────────────────

bool MODPlayer::LoadPattern(uint8_t pattern) {
    uint8_t row, channel, i;
    uint8_t temp[4];
    uint16_t amigaPeriod;

    if (!file->seekSet(1084 + pattern * ROWS * Mod.numberOfChannels * 4)) return false;

    for (row = 0; row < ROWS; row++) {
        for (channel = 0; channel < Mod.numberOfChannels; channel++) {
            if (4 != file->read(temp, 4)) return false;

            Player.currentPattern.sampleNumber[row][channel] = (temp[0] & 0xF0) + (temp[2] >> 4);

            amigaPeriod = ((temp[0] & 0xF) << 8) + temp[1];
            Player.currentPattern.note8[row][channel] = NONOTE8;
            for (i = 1; i < 37; i++)
                if (amigaPeriod > ReadAmigaPeriods(i * 8) - 3 &&
                        amigaPeriod < ReadAmigaPeriods(i * 8) + 3) {
                    Player.currentPattern.note8[row][channel] = i;
                }

            Player.currentPattern.effectNumber[row][channel]    = temp[2] & 0xF;
            Player.currentPattern.effectParameter[row][channel] = temp[3];
        }
    }
    return true;
}

// ── Portamento — verbatim ─────────────────────────────────────────────────────

void MODPlayer::Portamento(uint8_t channel) {
    if (Player.lastAmigaPeriod[channel] < Player.portamentoNote[channel]) {
        Player.lastAmigaPeriod[channel] += Player.portamentoSpeed[channel];
        if (Player.lastAmigaPeriod[channel] > Player.portamentoNote[channel]) {
            Player.lastAmigaPeriod[channel] = Player.portamentoNote[channel];
        }
    }
    if (Player.lastAmigaPeriod[channel] > Player.portamentoNote[channel]) {
        Player.lastAmigaPeriod[channel] -= Player.portamentoSpeed[channel];
        if (Player.lastAmigaPeriod[channel] < Player.portamentoNote[channel]) {
            Player.lastAmigaPeriod[channel] = Player.portamentoNote[channel];
        }
    }
    Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
}

// ── Vibrato — verbatim ────────────────────────────────────────────────────────

void MODPlayer::Vibrato(uint8_t channel) {
    uint16_t delta;
    uint16_t temp;

    temp = Player.vibratoPos[channel] & 31;

    switch (Player.waveControl[channel] & 3) {
    case 0:
        delta = ReadSine(temp);
        break;
    case 1:
        temp <<= 3;
        if (Player.vibratoPos[channel] < 0) temp = 255 - temp;
        delta = temp;
        break;
    case 2:
        delta = 255;
        break;
    case 3:
        delta = rand() & 255;
        break;
    }

    delta *= Player.vibratoDepth[channel];
    delta >>= 7;

    if (Player.vibratoPos[channel] >= 0) {
        Mixer.channelFrequency[channel] = Player.amiga / (Player.lastAmigaPeriod[channel] + delta);
    } else {
        Mixer.channelFrequency[channel] = Player.amiga / (Player.lastAmigaPeriod[channel] - delta);
    }

    Player.vibratoPos[channel] += Player.vibratoSpeed[channel];
    if (Player.vibratoPos[channel] > 31) Player.vibratoPos[channel] -= 64;
}

// ── Tremolo — verbatim ────────────────────────────────────────────────────────

void MODPlayer::Tremolo(uint8_t channel) {
    uint16_t delta;
    uint16_t temp;

    temp = Player.tremoloPos[channel] & 31;

    switch (Player.waveControl[channel] & 3) {
    case 0:
        delta = ReadSine(temp);
        break;
    case 1:
        temp <<= 3;
        if (Player.tremoloPos[channel] < 0) temp = 255 - temp;
        delta = temp;
        break;
    case 2:
        delta = 255;
        break;
    case 3:
        delta = rand() & 255;
        break;
    }

    delta *= Player.tremoloDepth[channel];
    delta >>= 6;

    if (Player.tremoloPos[channel] >= 0) {
        if (Player.volume[channel] + delta > 64) delta = 64 - Player.volume[channel];
        Mixer.channelVolume[channel] = Player.volume[channel] + delta;
    } else {
        if (Player.volume[channel] - delta < 0) delta = Player.volume[channel];
        Mixer.channelVolume[channel] = Player.volume[channel] - delta;
    }

    Player.tremoloPos[channel] += Player.tremoloSpeed[channel];
    if (Player.tremoloPos[channel] > 31) Player.tremoloPos[channel] -= 64;
}

// ── ProcessRow — verbatim ─────────────────────────────────────────────────────

bool MODPlayer::ProcessRow() {
    bool jumpFlag;
    bool breakFlag;
    uint8_t channel;
    uint8_t sampleNumber;
    uint16_t note;
    uint8_t effectNumber;
    uint8_t effectParameter;
    uint8_t effectParameterX;
    uint8_t effectParameterY;
    uint16_t sampleOffset;

    if (!running) return false;

    Player.lastRow = Player.row++;
    jumpFlag  = false;
    breakFlag = false;

    for (channel = 0; channel < Mod.numberOfChannels; channel++) {
        sampleNumber    = Player.currentPattern.sampleNumber[Player.lastRow][channel];
        note            = NOTE(Player.lastRow, channel);
        effectNumber    = Player.currentPattern.effectNumber[Player.lastRow][channel];
        effectParameter = Player.currentPattern.effectParameter[Player.lastRow][channel];
        effectParameterX = effectParameter >> 4;
        effectParameterY = effectParameter & 0xF;
        sampleOffset    = 0;

        if (sampleNumber) {
            Player.lastSampleNumber[channel] = sampleNumber - 1;
            if (!(effectNumber == 0xE && effectParameterX == NOTEDELAY)) {
                Player.volume[channel] = Mod.samples[Player.lastSampleNumber[channel]].volume;
            }
        }

        if (note != NONOTE) {
            Player.lastNote[channel] = note;
            Player.amigaPeriod[channel] = ReadAmigaPeriods(note + Mod.samples[Player.lastSampleNumber[channel]].fineTune);
            if (effectNumber != TONEPORTAMENTO && effectNumber != PORTAMENTOVOLUMESLIDE) {
                Player.lastAmigaPeriod[channel] = Player.amigaPeriod[channel];
            }
            if (!(Player.waveControl[channel] & 0x80)) Player.vibratoPos[channel] = 0;
            if (!(Player.waveControl[channel] & 0x08)) Player.tremoloPos[channel] = 0;
        }

        switch (effectNumber) {
        case TONEPORTAMENTO:
            if (effectParameter) Player.portamentoSpeed[channel] = effectParameter;
            Player.portamentoNote[channel] = Player.amigaPeriod[channel];
            note = NONOTE;
            break;

        case VIBRATO:
            if (effectParameterX) Player.vibratoSpeed[channel] = effectParameterX;
            if (effectParameterY) Player.vibratoDepth[channel] = effectParameterY;
            break;

        case PORTAMENTOVOLUMESLIDE:
            Player.portamentoNote[channel] = Player.amigaPeriod[channel];
            note = NONOTE;
            break;

        case TREMOLO:
            if (effectParameterX) Player.tremoloSpeed[channel] = effectParameterX;
            if (effectParameterY) Player.tremoloDepth[channel] = effectParameterY;
            break;

        case SETCHANNELPANNING:
            Mixer.channelPanning[channel] = effectParameter >> 1;
            break;

        case SETSAMPLEOFFSET:
            sampleOffset = effectParameter << 8;
            if (sampleOffset > Mod.samples[Player.lastSampleNumber[channel]].length) {
                sampleOffset = Mod.samples[Player.lastSampleNumber[channel]].length;
            }
            break;

        case JUMPTOORDER:
            Player.orderIndex = effectParameter;
            if (Player.orderIndex >= Mod.songLength) Player.orderIndex = 0;
            Player.row = 0;
            jumpFlag = true;
            break;

        case SETVOLUME:
            if (effectParameter > 64) Player.volume[channel] = 64;
            else                      Player.volume[channel] = effectParameter;
            break;

        case BREAKPATTERNTOROW:
            Player.row = effectParameterX * 10 + effectParameterY;
            if (Player.row >= ROWS) Player.row = 0;
            if (!jumpFlag && !breakFlag) {
                Player.orderIndex++;
                if (Player.orderIndex >= Mod.songLength) Player.orderIndex = 0;
            }
            breakFlag = true;
            break;

        case 0xE:
            switch (effectParameterX) {
            case FINEPORTAMENTOUP:
                Player.lastAmigaPeriod[channel] -= effectParameterY;
                break;
            case FINEPORTAMENTODOWN:
                Player.lastAmigaPeriod[channel] += effectParameterY;
                break;
            case SETVIBRATOWAVEFORM:
                Player.waveControl[channel] &= 0xF0;
                Player.waveControl[channel] |= effectParameterY;
                break;
            case SETFINETUNE:
                Mod.samples[Player.lastSampleNumber[channel]].fineTune = effectParameterY;
                if (Mod.samples[Player.lastSampleNumber[channel]].fineTune > 7)
                    Mod.samples[Player.lastSampleNumber[channel]].fineTune -= 16;
                break;
            case PATTERNLOOP:
                if (effectParameterY) {
                    if (Player.patternLoopCount[channel]) {
                        Player.patternLoopCount[channel]--;
                    } else {
                        Player.patternLoopCount[channel] = effectParameterY;
                    }
                    if (Player.patternLoopCount[channel]) {
                        Player.row = Player.patternLoopRow[channel] - 1;
                    }
                } else {
                    Player.patternLoopRow[channel] = Player.row;
                }
                break;
            case SETTREMOLOWAVEFORM:
                Player.waveControl[channel] &= 0xF;
                Player.waveControl[channel] |= effectParameterY << 4;
                break;
            case FINEVOLUMESLIDEUP:
                Player.volume[channel] += effectParameterY;
                if (Player.volume[channel] > 64) Player.volume[channel] = 64;
                break;
            case FINEVOLUMESLIDEDOWN:
                Player.volume[channel] -= effectParameterY;
                if (Player.volume[channel] < 0) Player.volume[channel] = 0;
                break;
            case NOTECUT:
                note = NONOTE;
                break;
            case PATTERNDELAY:
                Player.patternDelay = effectParameterY;
                break;
            case INVERTLOOP:
                break;
            }
            break;

        case SETSPEED:
            if (effectParameter < 0x20) {
                Player.speed = effectParameter;
            } else {
                Player.samplesPerTick = sampleRate / (2 * effectParameter / 5);
            }
            break;
        }

        if (note != NONOTE || (Player.lastAmigaPeriod[channel] &&
                effectNumber != VIBRATO && effectNumber != VIBRATOVOLUMESLIDE &&
                !(effectNumber == 0xE && effectParameterX == NOTEDELAY))) {
            Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
        }

        if (note != NONOTE) {
            Mixer.channelSampleOffset[channel] = sampleOffset << FIXED_DIVIDER;
        }

        if (sampleNumber) {
            Mixer.channelSampleNumber[channel] = Player.lastSampleNumber[channel];
        }

        if (effectNumber != TREMOLO) {
            Mixer.channelVolume[channel] = Player.volume[channel];
        }
    }

    updateUI();
    return true;
}

// ── ProcessTick — verbatim ────────────────────────────────────────────────────

bool MODPlayer::ProcessTick() {
    uint8_t channel;
    uint8_t sampleNumber;
    uint16_t note;
    uint8_t effectNumber;
    uint8_t effectParameter;
    uint8_t effectParameterX;
    uint8_t effectParameterY;
    uint16_t tempNote;

    if (!running) return false;

    for (channel = 0; channel < Mod.numberOfChannels; channel++) {
        if (Player.lastAmigaPeriod[channel]) {
            sampleNumber    = Player.currentPattern.sampleNumber[Player.lastRow][channel];
            note            = NOTE(Player.lastRow, channel);
            effectNumber    = Player.currentPattern.effectNumber[Player.lastRow][channel];
            effectParameter = Player.currentPattern.effectParameter[Player.lastRow][channel];
            effectParameterX = effectParameter >> 4;
            effectParameterY = effectParameter & 0xF;

            switch (effectNumber) {
            case ARPEGGIO:
                if (effectParameter)
                    switch (Player.tick % 3) {
                    case 0:
                        Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
                        break;
                    case 1:
                        tempNote = Player.lastNote[channel] + effectParameterX * 8 + Mod.samples[Player.lastSampleNumber[channel]].fineTune;
                        if (tempNote < 296) Mixer.channelFrequency[channel] = Player.amiga / ReadAmigaPeriods(tempNote);
                        break;
                    case 2:
                        tempNote = Player.lastNote[channel] + effectParameterY * 8 + Mod.samples[Player.lastSampleNumber[channel]].fineTune;
                        if (tempNote < 296) Mixer.channelFrequency[channel] = Player.amiga / ReadAmigaPeriods(tempNote);
                        break;
                    }
                break;

            case PORTAMENTOUP:
                Player.lastAmigaPeriod[channel] -= effectParameter;
                if (Player.lastAmigaPeriod[channel] < 113) Player.lastAmigaPeriod[channel] = 113;
                Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
                break;

            case PORTAMENTODOWN:
                Player.lastAmigaPeriod[channel] += effectParameter;
                if (Player.lastAmigaPeriod[channel] > 856) Player.lastAmigaPeriod[channel] = 856;
                Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
                break;

            case TONEPORTAMENTO:
                Portamento(channel);
                break;

            case VIBRATO:
                Vibrato(channel);
                break;

            case PORTAMENTOVOLUMESLIDE:
                Portamento(channel);
                Player.volume[channel] += effectParameterX - effectParameterY;
                if (Player.volume[channel] < 0)       Player.volume[channel] = 0;
                else if (Player.volume[channel] > 64) Player.volume[channel] = 64;
                Mixer.channelVolume[channel] = Player.volume[channel];
                break;

            case VIBRATOVOLUMESLIDE:
                Vibrato(channel);
                Player.volume[channel] += effectParameterX - effectParameterY;
                if (Player.volume[channel] < 0)       Player.volume[channel] = 0;
                else if (Player.volume[channel] > 64) Player.volume[channel] = 64;
                Mixer.channelVolume[channel] = Player.volume[channel];
                break;

            case TREMOLO:
                Tremolo(channel);
                break;

            case VOLUMESLIDE:
                Player.volume[channel] += effectParameterX - effectParameterY;
                if (Player.volume[channel] < 0)       Player.volume[channel] = 0;
                else if (Player.volume[channel] > 64) Player.volume[channel] = 64;
                Mixer.channelVolume[channel] = Player.volume[channel];
                break;

            case 0xE:
                switch (effectParameterX) {
                case RETRIGGERNOTE:
                    if (!effectParameterY) break;
                    if (!(Player.tick % effectParameterY)) Mixer.channelSampleOffset[channel] = 0;
                    break;

                case NOTECUT:
                    if (Player.tick == effectParameterY) {
                        Mixer.channelVolume[channel] = Player.volume[channel] = 0;
                    }
                    break;

                case NOTEDELAY:
                    if (Player.tick == effectParameterY) {
                        if (sampleNumber) {
                            Player.volume[channel] = Mod.samples[Player.lastSampleNumber[channel]].volume;
                        }
                        if (note != NONOTE) {
                            Mixer.channelSampleOffset[channel] = 0;
                        }
                        Mixer.channelFrequency[channel] = Player.amiga / Player.lastAmigaPeriod[channel];
                        Mixer.channelVolume[channel] = Player.volume[channel];
                    }
                    break;
                }
                break;
            }
        }
    }
    return true;
}

// ── RunPlayer — verbatim + updateUI call ─────────────────────────────────────

bool MODPlayer::RunPlayer() {
    if (!running) return false;

    if (Player.tick == Player.speed) {
        Player.tick = 0;

        if (Player.row == ROWS) {
            Player.orderIndex++;
            if (Player.orderIndex == Mod.songLength) return false;
            Player.row = 0;
        }

        if (Player.patternDelay) {
            Player.patternDelay--;
        } else {
            if (Player.orderIndex != Player.oldOrderIndex)
                if (!LoadPattern(Mod.order[Player.orderIndex])) return false;
            Player.oldOrderIndex = Player.orderIndex;
            if (!ProcessRow()) return false;  // updateUI called inside ProcessRow
        }
    } else {
        if (!ProcessTick()) return false;
    }
    Player.tick++;
    return true;
}

// ── GetSample — verbatim, file->seek/read replaced with SdFat ────────────────

void MODPlayer::GetSample(int16_t sample[2]) {
    int32_t sumL;
    int32_t sumR;
    uint8_t channel;
    uint32_t samplePointer;
    int8_t current;
    int8_t next;
    int16_t out;
    int32_t out32;

    if (!running) return;

    sumL = 0;
    sumR = 0;

    for (channel = 0; channel < Mod.numberOfChannels; channel++) {
        if (!Mixer.channelFrequency[channel] ||
                !Mod.samples[Mixer.channelSampleNumber[channel]].length) {
            continue;
        }

        Mixer.channelSampleOffset[channel] += Mixer.channelFrequency[channel];

        if (!Mixer.channelVolume[channel]) continue;

        samplePointer = Mixer.sampleBegin[Mixer.channelSampleNumber[channel]] +
                        (Mixer.channelSampleOffset[channel] >> FIXED_DIVIDER);

        if (Mixer.sampleLoopLength[Mixer.channelSampleNumber[channel]]) {
            if (samplePointer >= Mixer.sampleLoopEnd[Mixer.channelSampleNumber[channel]]) {
                Mixer.channelSampleOffset[channel] -= Mixer.sampleLoopLength[Mixer.channelSampleNumber[channel]] << FIXED_DIVIDER;
                samplePointer -= Mixer.sampleLoopLength[Mixer.channelSampleNumber[channel]];
            }
        } else {
            if (samplePointer >= Mixer.sampleEnd[Mixer.channelSampleNumber[channel]]) {
                Mixer.channelFrequency[channel] = 0;
                samplePointer = Mixer.sampleEnd[Mixer.channelSampleNumber[channel]];
            }
        }

        // SdFat lookahead cache — replaces AudioFileSource seek/read
        if (samplePointer < FatBuffer.samplePointer[channel] ||
                samplePointer >= FatBuffer.samplePointer[channel] + (uint32_t)fatBufferSize - 1 ||
                Mixer.channelSampleNumber[channel] != FatBuffer.channelSampleNumber[channel]) {

            uint32_t toRead = Mixer.sampleEnd[Mixer.channelSampleNumber[channel]] - samplePointer + 1;
            if (toRead > (uint32_t)fatBufferSize) toRead = fatBufferSize;

            file->seekSet(samplePointer);
            file->read(FatBuffer.channels[channel], toRead);

            FatBuffer.samplePointer[channel]       = samplePointer;
            FatBuffer.channelSampleNumber[channel] = Mixer.channelSampleNumber[channel];
        }

        current = (int8_t)FatBuffer.channels[channel][(samplePointer - FatBuffer.samplePointer[channel])];
        next    = (int8_t)FatBuffer.channels[channel][(samplePointer + 1 - FatBuffer.samplePointer[channel])];

        int16_t current16 = (int16_t)current << 2;
        int16_t next16    = (int16_t)next    << 2;

        out = current16;
        out += (next16 - current16) * (Mixer.channelSampleOffset[channel] & ((1 << FIXED_DIVIDER) - 1)) >> FIXED_DIVIDER;

        out32 = (int32_t)out << (BITDEPTH - 10);
        out32 = out32 * Mixer.channelVolume[channel] >> 6;

        sumL += out32 * min(128 - Mixer.channelPanning[channel], 64) >> 6;
        sumR += out32 * min(Mixer.channelPanning[channel], 64)       >> 6;
    }

    if (Mod.numberOfChannels <= 4) {
        sumL /= 4;
        sumR /= 4;
    } else {
        if (Mod.numberOfChannels <= 6) {
            sumL = (sumL + (sumL / 2)) / 8;
            sumR = (sumR + (sumR / 2)) / 8;
        } else {
            sumL /= 8;
            sumR /= 8;
        }
    }

    if (sumL <= INT16_MIN) sumL = INT16_MIN; else if (sumL >= INT16_MAX) sumL = INT16_MAX;
    if (sumR <= INT16_MIN) sumR = INT16_MIN; else if (sumR >= INT16_MAX) sumR = INT16_MAX;
    waveRing[wavePos % 182] = (int16_t)((sumL + sumR) / 2);
    wavePos++;
    sample[0] = sumL; // LEFT
    sample[1] = sumR; // RIGHT
}

// ── LoadMOD — verbatim ────────────────────────────────────────────────────────

bool MODPlayer::LoadMOD() {
    uint8_t channel;

    if (!LoadHeader()) return false;
    LoadSamples();

    Player.amiga         = AMIGA;
    Player.samplesPerTick = sampleRate / (2 * 125 / 5);
    Player.speed         = 6;
    Player.tick          = Player.speed;
    Player.row           = 0;
    Player.orderIndex    = 0;
    Player.oldOrderIndex = 0xFF;
    Player.patternDelay  = 0;

    for (channel = 0; channel < Mod.numberOfChannels; channel++) {
        Player.patternLoopCount[channel] = 0;
        Player.patternLoopRow[channel]   = 0;
        Player.lastAmigaPeriod[channel]  = 0;
        Player.waveControl[channel]      = 0;
        Player.vibratoSpeed[channel]     = 0;
        Player.vibratoDepth[channel]     = 0;
        Player.vibratoPos[channel]       = 0;
        Player.tremoloSpeed[channel]     = 0;
        Player.tremoloDepth[channel]     = 0;
        Player.tremoloPos[channel]       = 0;

        FatBuffer.samplePointer[channel]       = 0;
        FatBuffer.channelSampleNumber[channel] = 0xFF;

        Mixer.channelSampleOffset[channel] = 0;
        Mixer.channelFrequency[channel]    = 0;
        Mixer.channelVolume[channel]       = 0;

        switch (channel % 4) {
        case 0:
        case 3:
            Mixer.channelPanning[channel] = stereoSeparation;
            break;
        default:
            Mixer.channelPanning[channel] = 128 - stereoSeparation;
        }
    }
    return true;
}
