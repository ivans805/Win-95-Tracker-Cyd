#include <SdFat.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "MODPlayer.h"
#include "bitmaps.h"

// ── I2S config ────────────────────────────────────────────────────────────────
#define I2S_BCK     27
#define I2S_WS      22
#define I2S_DATA     4
#define SAMPLE_RATE 44100

// ── SD config ─────────────────────────────────────────────────────────────────
#define SD_CS    5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

// ── Touch config ──────────────────────────────────────────────────────────────
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// ── Colors ────────────────────────────────────────────────────────────────────
#define COL_BG         0x0000
#define COL_GREEN      0x07E0
#define COL_WHITE      0xFFFF
#define COL_BLUE       0x001F
#define COL_HILITE     0xFFFF
#define COL_HILITE_TXT 0x0000

// ── Tracker layout ────────────────────────────────────────────────────────────
#define TRACKER_X     22
#define TRACKER_Y     23
#define TRACKER_ROW_H 10
#define TRACKER_ROWS  14
#define TRACKER_MID    6
#define TRACKER_W    280

// ── Oscilloscope layout ───────────────────────────────────────────────────────
#define WAVE_X      130
#define WAVE_Y      213
#define WAVE_W      182
#define WAVE_H       22
#define WAVE_MID    (WAVE_Y + WAVE_H / 2)

// ── Filename scroll ───────────────────────────────────────────────────────────
#define FNAME_MAX_VISIBLE 24
#define FNAME_SCROLL_MS  150

// ── Button hit regions ────────────────────────────────────────────────────────
#define BTN_PREV_X   10
#define BTN_PREV_Y  210
#define BTN_PREV_W   38
#define BTN_PREV_H   28

#define BTN_PLAY_X   50
#define BTN_PLAY_Y  210
#define BTN_PLAY_W   38
#define BTN_PLAY_H   28

#define BTN_NEXT_X   90
#define BTN_NEXT_Y  210
#define BTN_NEXT_W   38
#define BTN_NEXT_H   28

#define BOING_FRAME_COUNT 11
#define BOING_FRAME_W     18
#define BOING_FRAME_H     18
#define BOING_STRIDE      20  // 18px + 2px gap
#define BOING_INTERVAL    40  // 25fps = 40ms per frame

String Version = "Beta V0.4";
String Decoder = "Esp8266 Mod Generator";

SdFat    sd;
SdFile   file;
SdFile   root;
TFT_eSPI tft;
MODPlayer player(I2S_NUM_0);

SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ── State ─────────────────────────────────────────────────────────────────────
char          modName[64]    = "";
char          scrollBuf[132] = "";
int           scrollOffset   = 0;
unsigned long lastScroll     = 0;
unsigned long songStart      = 0;
bool          isPlaying      = true;

// button held state
bool prevHeld = false;
bool playHeld = false;
bool nextHeld = false;

static int           barPos  = 0;
static unsigned long lastBar = 0;

int currentModIndex = 0;
int modCount        = 0;   // total MOD files on SD, counted once at boot

unsigned long pausedElapsed        = 0;
bool          pauseModTrackerState = false;

static const char* noteNames[] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

int boingFrame = 0;
uint32_t lastBoingTick = 0;

uint16_t boingBuf[18 * 18];

// ── Draw one boing ball frame ─────────────────────────────────────────────────
void drawBoingBall(int screenX, int screenY, int frame) {
    int stripWidth = 218;
    int srcX = frame * BOING_STRIDE;
    for (int row = 0; row < BOING_FRAME_H; row++) {
        for (int col = 0; col < BOING_FRAME_W; col++) {
            boingBuf[row * BOING_FRAME_W + col] =
                pgm_read_word(&image_boing_pixels[row * stripWidth + srcX + col]);
        }
    }
    tft.pushImage(screenX, screenY, BOING_FRAME_W, BOING_FRAME_H, boingBuf, 0x0000);
}

// ── Count total MOD files on SD (called once at boot) ─────────────────────────
int countMods() {
    root.rewind();
    SdFile f;
    int count = 0;
    while (f.openNext(&root, O_RDONLY)) {
        char name[64];
        f.getName(name, sizeof(name));
        if (strstr(name, ".mod") || strstr(name, ".MOD")) count++;
        f.close();
    }
    root.rewind();
    return count;
}

// ── Open the Nth .mod file (0-based) from directory start ─────────────────────
bool openModAtIndex(int targetIndex) {
    tft.fillRect(121, 191, 181, 17, 0x738E);
    tft.setTextSize(2);
    tft.setTextColor(0x4208);
    tft.drawString("Loading...", 123, 192);
    for (int i = 0; i < 11; i++) {
        drawBoingBall(99, 190, i % BOING_FRAME_COUNT);
        delay(40);
    }
    file.close();
    root.rewind();
    int count = 0;
    while (true) {
        if (!file.openNext(&root, O_RDONLY)) return false;
        char name[64];
        file.getName(name, sizeof(name));
        if (strstr(name, ".mod") || strstr(name, ".MOD")) {
            if (count == targetIndex) {
                strncpy(modName, name, 63);
                modName[63] = 0;
                snprintf(scrollBuf, 132, "%s   %s", modName, modName);
                scrollOffset = 0;
                lastScroll   = millis();
                return true;
            }
            count++;
        }
        file.close();
    }
}

// ── Configure and start player ────────────────────────────────────────────────
void startPlaying() {
    player.SetSampleRate(SAMPLE_RATE);
    player.SetBufferSize(6 * 1024);
    player.SetStereoSeparation(32);
    if (!player.begin(file)) Serial.println("MODPlayer begin failed!");
    songStart            = millis();
    pausedElapsed        = 0;
    scrollOffset         = 0;
    isPlaying            = true;
    pauseModTrackerState = false;
    drawFilename();
    tft.fillRect(121, 191, 181, 17, 0x738E);
    drawBoingBall(99, 190, 1 % BOING_FRAME_COUNT);
    tft.setTextSize(2);
    tft.setTextColor(0x4208);
    tft.drawString("Ready: Playing", 123, 192);
}

// ── Build one tracker row string ──────────────────────────────────────────────
void buildRowString(char* buf, int patternRow) {
    if (patternRow < 0 || patternRow >= 64) { buf[0] = 0; return; }
    char tmp[12];
    snprintf(buf, 8, "%02d|", patternRow);
    for (int ch = 0; ch < player.ui.numChannels && ch < 4; ch++) {
        uint8_t note8  = player.Player.currentPattern.note8[patternRow][ch];
        uint8_t samp   = player.Player.currentPattern.sampleNumber[patternRow][ch];
        uint8_t effNum = player.Player.currentPattern.effectNumber[patternRow][ch];
        uint8_t effPar = player.Player.currentPattern.effectParameter[patternRow][ch];

        if (note8 == 0xFF) strcat(buf, "... ");
        else {
            snprintf(tmp, 8, "%s%d ", noteNames[(note8-1)%12], (note8-1)/12+1);
            strcat(buf, tmp);
        }
        if (samp == 0) strcat(buf, ".. ");
        else { snprintf(tmp, 8, "%02X ", samp); strcat(buf, tmp); }
        if (effNum == 0 && effPar == 0) strcat(buf, "...|");
        else { snprintf(tmp, 8, "%X%02X|", effNum, effPar); strcat(buf, tmp); }
    }
}

// ── Draw one tracker slot ─────────────────────────────────────────────────────
void drawTrackerSlot(int slot, int patternRow, bool highlight) {
    tft.setTextSize(1);
    tft.setFreeFont();
    int y = TRACKER_Y + slot * TRACKER_ROW_H;
    char buf[64];
    buildRowString(buf, patternRow);

    if (highlight) {
        tft.fillRect(TRACKER_X - 1, y - 1, TRACKER_W, TRACKER_ROW_H, COL_HILITE);
        tft.setTextColor(COL_HILITE_TXT);
    } else {
        tft.fillRect(TRACKER_X - 1, y - 1, TRACKER_W, TRACKER_ROW_H, COL_BG);
        tft.setTextColor(COL_GREEN);
    }
    if (buf[0]) tft.drawString(buf, TRACKER_X, y);
}

// ── Draw all 14 tracker rows ──────────────────────────────────────────────────
void drawTracker() {
    int cur = player.ui.row;
    for (int slot = 0; slot < TRACKER_ROWS; slot++) {
        drawTrackerSlot(slot, cur - TRACKER_MID + slot, slot == TRACKER_MID);
    }
    tft.drawLine(20, 162, 302, 162, 0x8C71);

    int noteX[4]   = { 38, 104, 170, 236 };
    int sampleX[4] = { 62, 128, 194, 260 };
    int effectX[4] = { 80, 146, 212, 278 };

    for (int ch = 0; ch < 4; ch++) {
        uint8_t note8  = player.Player.currentPattern.note8[cur][ch];
        uint8_t samp   = player.Player.currentPattern.sampleNumber[cur][ch];
        uint8_t effNum = player.Player.currentPattern.effectNumber[cur][ch];
        uint8_t effPar = player.Player.currentPattern.effectParameter[cur][ch];

        if (note8 != 0xFF)
            tft.drawRect(noteX[ch], 21, 21, 140, 0xFFE0);
        else
            tft.drawRect(noteX[ch], 21, 21, 140, COL_BG);

        if (samp != 0)
            tft.drawRect(sampleX[ch], 21, 15, 140, 0xF800);
        else
            tft.drawRect(sampleX[ch], 21, 15, 140, COL_BG);

        if (effNum != 0 || effPar != 0)
            tft.drawRect(effectX[ch], 21, 21, 140, 0x24BE);
        else
            tft.drawRect(effectX[ch], 21, 21, 140, COL_BG);
    }
}

// ── Update top bar values ─────────────────────────────────────────────────────
void drawTopBar() {
    char buf[16];
    tft.setTextSize(1);
    tft.setFreeFont();
    tft.setTextColor(COL_GREEN);

    tft.fillRect(62, 5, 43, 9, COL_BG);
    snprintf(buf, 16, "%03d/%03d",
        min((int)player.ui.orderIndex, (int)player.Mod.songLength - 1),
        (int)player.Mod.songLength);
    tft.drawString(buf, 63, 6);

    tft.fillRect(146, 5, 43, 9, COL_BG);
    snprintf(buf, 16, "%03d/063", player.ui.row);
    tft.drawString(buf, 147, 6);

    tft.fillRect(230, 5, 19, 9, COL_BG);
    snprintf(buf, 16, "%03d", player.ui.bpm);
    tft.drawString(buf, 231, 6);

    tft.fillRect(293, 5, 7, 9, COL_BG);
    snprintf(buf, 4, "%d", player.ui.speed);
    tft.drawString(buf, 294, 6);
}

// ── Draw filename with scroll if needed ──────────────────────────────────────
void drawFilename() {
    tft.fillRect(19, 167, 288, 16, COL_BG);
    tft.setTextColor(COL_GREEN, 0x000000, true);
    tft.setTextSize(2);
    tft.setFreeFont();
    if ((int)strlen(modName) <= FNAME_MAX_VISIBLE) {
        tft.drawString(modName, 20, 168);
    } else {
        char window[FNAME_MAX_VISIBLE + 1];
        strncpy(window, scrollBuf + scrollOffset, FNAME_MAX_VISIBLE);
        window[FNAME_MAX_VISIBLE] = 0;
        tft.drawString(window, 20, 168);
    }
}

// ── Advance filename scroll offset ───────────────────────────────────────────
void updateScroll() {
    if ((int)strlen(modName) <= FNAME_MAX_VISIBLE) return;
    if (millis() - lastScroll < FNAME_SCROLL_MS) return;
    lastScroll = millis();
    scrollOffset++;
    if (scrollOffset >= (int)(strlen(modName) + 3))
        scrollOffset = 0;
    drawFilename();
    tft.drawLine(307, 167, 307, 184, 0x8C71);
}

// ── Draw elapsed time ─────────────────────────────────────────────────────────
void drawTime() {
    unsigned long elapsed = (isPlaying ? (millis() - songStart) : pausedElapsed) / 1000;
    char buf[8];
    snprintf(buf, 8, "%02d:%02d", (int)(elapsed / 60), (int)(elapsed % 60));
    tft.fillRect(19, 191, 60, 16, COL_BG);
    tft.setTextColor(COL_GREEN);
    tft.setTextSize(2);
    tft.setFreeFont();
    tft.drawString(buf, 20, 192);
}

// ── Draw oscilloscope waveform ────────────────────────────────────────────────
void drawWave() {
    tft.fillRect(WAVE_X, WAVE_Y, WAVE_W, WAVE_H, COL_BG);

    int16_t wmin = 32767, wmax = -32768;
    for (int i = 0; i < WAVE_W; i++) {
        int16_t v = player.waveRing[i];
        if (v < wmin) wmin = v;
        if (v > wmax) wmax = v;
    }
    int range = wmax - wmin;
    if (range < 1) range = 1;

    for (int x = 0; x < WAVE_W - 1; x++) {
        int idx  = (player.wavePos + x)     % WAVE_W;
        int idx2 = (player.wavePos + x + 1) % WAVE_W;

        int y1 = WAVE_Y + WAVE_H - 1 - ((player.waveRing[idx]  - wmin) * (WAVE_H - 1) / range);
        int y2 = WAVE_Y + WAVE_H - 1 - ((player.waveRing[idx2] - wmin) * (WAVE_H - 1) / range);

        y1 = constrain(y1, WAVE_Y, WAVE_Y + WAVE_H - 1);
        y2 = constrain(y2, WAVE_Y, WAVE_Y + WAVE_H - 1);

        tft.drawLine(WAVE_X + x, y1, WAVE_X + x + 1, y2, COL_GREEN);
    }
}

// ── Draw prev button ─────────────────────────────────────────────────────────
void drawBtnPrev(bool held) {
    int off = held ? 1 : 0;
    if (held)
        tft.pushImage(BTN_PREV_X, BTN_PREV_Y, BTN_PREV_W, BTN_PREV_H, image_btn_background_inverted);
    else
        tft.pushImage(BTN_PREV_X, BTN_PREV_Y, BTN_PREV_W, BTN_PREV_H, image_btn_background_normal);
    tft.drawBitmap(21 + off, 217 + off, image_music_previous_bits, 16, 14, 0x4229);
}

// ── Draw play/pause button ───────────────────────────────────────────────────
void drawBtnPlay(bool held) {
    int off = held ? 1 : 0;
    if (held)
        tft.pushImage(BTN_PLAY_X, BTN_PLAY_Y, BTN_PLAY_W, BTN_PLAY_H, image_btn_background_inverted);
    else
        tft.pushImage(BTN_PLAY_X, BTN_PLAY_Y, BTN_PLAY_W, BTN_PLAY_H, image_btn_background_normal);
    if (isPlaying)
        tft.drawBitmap(63 + off, 216 + off, image_music_pause_bits, 12, 15, 0x4249);
    else
        tft.drawBitmap(61 + off, 216 + off, image_music_play_bits, 15, 15, 0x4249);
}

// ── Draw next button ─────────────────────────────────────────────────────────
void drawBtnNext(bool held) {
    int off = held ? 1 : 0;
    if (held)
        tft.pushImage(BTN_NEXT_X, BTN_NEXT_Y, BTN_NEXT_W, BTN_NEXT_H, image_btn_background_inverted);
    else
        tft.pushImage(BTN_NEXT_X, BTN_NEXT_Y, BTN_NEXT_W, BTN_NEXT_H, image_btn_background_normal);
    tft.drawBitmap(101 + off, 217 + off, image_music_next_bits, 16, 14, 0x4229);
}

// ── Static UI frame ───────────────────────────────────────────────────────────
void drawFrame() {
    tft.fillScreen(0x738E);
    tft.drawLine(130, 223, 311, 223, 0x7E0);
    tft.drawLine(302, 20, 302, 162, 0x8C71);
    tft.drawLine(302, 19, 20, 19, 0x4208);
    tft.drawLine(19, 19, 19, 162, 0x4208);
    tft.drawLine(106, 4, 106, 15, 0x8C71);
    tft.drawLine(105, 15, 61, 15, 0x8C71);
    tft.drawLine(106, 3, 61, 3, 0x4208);
    tft.drawLine(60, 3, 60, 15, 0x4208);
    tft.drawLine(190, 4, 190, 14, 0x8C71);
    tft.drawLine(145, 15, 190, 15, 0x8C71);
    tft.drawLine(229, 15, 249, 15, 0x8C71);
    tft.drawLine(250, 4, 250, 15, 0x8C71);
    tft.drawLine(292, 15, 301, 15, 0x8C71);
    tft.drawLine(301, 4, 301, 14, 0x8C71);
    tft.drawLine(144, 15, 144, 3, 0x4208);
    tft.drawLine(145, 3, 190, 3, 0x4208);
    tft.drawLine(228, 15, 228, 3, 0x4208);
    tft.drawLine(229, 3, 250, 3, 0x4208);
    tft.drawLine(291, 15, 291, 4, 0x4208);
    tft.drawLine(291, 3, 301, 3, 0x4208);
    tft.drawLine(19, 185, 307, 185, 0x8C71);
    tft.drawLine(307, 166, 19, 166, 0x4208);
    tft.drawLine(18, 166, 18, 185, 0x4208);
    tft.drawLine(19, 207, 79, 207, 0x8C71);
    tft.drawLine(79, 191, 79, 206, 0x8C71);
    tft.drawLine(130, 235, 312, 235, 0x8C71);
    tft.drawLine(312, 213, 312, 234, 0x8C71);
    tft.drawLine(18, 190, 18, 207, 0x4208);
    tft.drawLine(129, 235, 129, 212, 0x4208);
    tft.drawLine(130, 212, 312, 212, 0x4208);
    tft.fillRect(20, 20, 282, 142, 0x0);
    tft.fillRect(145, 4, 45, 11, 0x0);
    tft.fillRect(229, 4, 21, 11, 0x0);
    tft.fillRect(61, 4, 45, 11, 0x0);
    tft.fillRect(292, 4, 9, 11, 0x0);
    tft.fillRect(19, 167, 288, 18, 0x0);
    tft.fillRect(19, 191, 60, 15, 0x0);
    tft.fillRect(130, 213, 182, 22, 0x0);
    tft.drawLine(79, 190, 18, 190, 0x4208);
    tft.setTextColor(0x4208);
    tft.setTextSize(2);
    tft.drawString("Row", 108, 2);
    tft.drawString("BMP", 193, 2);
    tft.drawString("Spd", 254, 2);
    tft.drawString("Pos", 22, 2);

    drawBtnPrev(false);
    drawBtnPlay(false);
    drawBtnNext(false);
}

// ── Touch helper ──────────────────────────────────────────────────────────────
bool touchInRect(int tx, int ty, int x, int y, int w, int h) {
    return tx >= x && tx <= x + w && ty >= y && ty <= y + h;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(0x738E);
    // Boot screen
    tft.drawRect(58, 58, 205, 122, 0xFFFF);
    tft.fillRect(60, 59, 202, 120, 0x8410);
    tft.drawLine(59, 59, 261, 59, 0xBDF7);
    tft.setTextColor(0xFFFF);
    tft.setTextSize(2);
    tft.setFreeFont();
    tft.drawString("Welcome!", 67, 66);
    tft.pushImage(195, 68, 50, 50, image_pixil_frame_0__3__pixels);
    tft.setTextSize(1);
    tft.drawString("To Win95 Tracker!", 67, 84);
    tft.drawString("Youre currently on:", 67, 93);
    tft.setTextColor(0x4208);
    tft.drawString(Version, 67, 102);
    tft.drawString(Decoder, 67, 111);
    tft.drawLine(261, 59, 261, 178, 0x4208);
    tft.setTextColor(0xFFFF);
    tft.setTextSize(2);
    tft.drawString("Loading...", 113, 133);
    tft.drawLine(261, 178, 59, 178, 0x4208);
    tft.drawLine(59, 177, 59, 59, 0xBDF7);
    tft.fillRect(64, 156, 192, 12, 0x0);
    tft.drawLine(64, 155, 255, 155, 0x4A49);
    tft.drawLine(63, 155, 63, 167, 0x4A49);
    tft.drawLine(63, 168, 255, 168, 0xBDF7);
    tft.drawLine(256, 155, 256, 168, 0xBDF7);

    for (int i = 0; i < 550; i++) {
        analogWrite(21, map(i, 0, 50, 0, 255));
        drawBoingBall(89, 131, i % BOING_FRAME_COUNT);

        static int pos = 0;
        static bool dir = true;
        static uint32_t lastMove = 0;

        if (millis() - lastMove > 40) {
            lastMove = millis();
            tft.fillRect(64, 156, 192, 12, 0x0);
            tft.fillRect(64 + pos,      157, 6, 10, 0x1F);
            tft.fillRect(64 + pos + 7,  157, 6, 10, 0x1F);
            tft.fillRect(64 + pos + 14, 157, 6, 10, 0x1F);
            if (dir) pos++;
            else     pos--;
            if (pos >= 172) dir = false;
            if (pos <= 0)   dir = true;
        }
        delay(40);
    }

    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);
    drawFrame();

    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num   = I2S_BCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_DATA,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, nullptr);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!sd.begin(SD_CS, SD_SCK_MHZ(25))) {
        tft.drawBitmap(101, 192, image_operation_forbidden_sign_bits, 15, 16, 0x4208);
        tft.fillRect(121, 191, 181, 17, 0x738E);
        tft.setTextSize(2);
        tft.setTextColor(0xF206);
        tft.drawString("Error: No SdCard", 123, 192);
        while(1) delay(1000);
    }

    root.openRoot(sd.vol());

    // Count files once — this is the source of truth for navigation
    modCount = countMods();
    if (modCount == 0) {
        tft.drawBitmap(101, 192, image_operation_forbidden_sign_bits, 15, 16, 0x4208);
        tft.fillRect(121, 191, 181, 17, 0x738E);
        tft.setTextSize(2);
        tft.setTextColor(0xF206);
        tft.drawString("Error: No Files", 123, 192);
        while(1) delay(1000);
    }

    currentModIndex = 0;
    openModAtIndex(currentModIndex);

    drawFrame();
    startPlaying();
    tft.setTextSize(1);
    Serial.println("########################################################################");
    Serial.println("# Welcome!                                                             #");
    Serial.println("# Your device has succesfully booted into Win95 Tracker                #");
    Serial.println("# The version that youre On:                                           #");
    Serial.println("# " + Version);
    Serial.println("# The Decoder Youre Currently using:                                   #");
    Serial.println("# " + Decoder);
    Serial.println("# The file that youre Playing:                                         #");
    Serial.print("# ");  Serial.println(modName);
    Serial.println("# Made by Ivan aka 3d_tech_guy on discord                              #");
    Serial.println("# Enjoy!                                                               #");
    Serial.println("########################################################################");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    if (!pauseModTrackerState) {
        if (!player.loop()) {
            // Song ended — auto advance to next
            file.close();
            currentModIndex = (currentModIndex + 1) % modCount;
            if (!openModAtIndex(currentModIndex)) {
                tft.fillRect(93, 187, 216, 22, 0x738E);
                tft.drawBitmap(101, 192, image_operation_forbidden_sign_bits, 15, 16, 0x4208);
                tft.setTextSize(2);
                tft.setTextColor(0xF206);
                tft.drawString("Error: No SdCard", 123, 192);
                while(1) delay(1000);
            }
            drawFrame();
            startPlaying();
            return;
        }
    }

    static uint8_t lastRow = 0xFF;
    if (player.ui.row != lastRow) {
        lastRow = player.ui.row;
        drawTracker();
        drawTopBar();
    }

    updateScroll();

    static unsigned long lastTimeDraw = 0;
    if (millis() - lastTimeDraw >= 1000) {
        lastTimeDraw = millis();
        drawTime();
    }

    static unsigned long lastWave = 0;
    if (millis() - lastWave >= 50) {
        lastWave = millis();
        drawWave();
    }

    // ── Touch handling ────────────────────────────────────────────────────────
    if (ts.tirqTouched() && ts.touched()) {
        TS_Point p = ts.getPoint();
        int tx = map(p.x, 200, 3800, 0, 320);
        int ty = map(p.y, 200, 3800, 0, 240);
        tft.drawPixel(tx, ty, 0x738E);
        if (touchInRect(tx, ty, BTN_PREV_X, BTN_PREV_Y, BTN_PREV_W, BTN_PREV_H) && !prevHeld) {
            prevHeld = true;
            drawBtnPrev(true);
        }
        if (touchInRect(tx, ty, BTN_PLAY_X, BTN_PLAY_Y, BTN_PLAY_W, BTN_PLAY_H) && !playHeld) {
            playHeld = true;
            drawBtnPlay(true);
        }
        if (touchInRect(tx, ty, BTN_NEXT_X, BTN_NEXT_Y, BTN_NEXT_W, BTN_NEXT_H) && !nextHeld) {
            nextHeld = true;
            drawBtnNext(true);
        }
    } else {
        // Finger lifted — fire actions

        if (prevHeld) {
            prevHeld = false;
            drawBtnPrev(false);
            file.close();
            currentModIndex = (currentModIndex - 1 + modCount) % modCount;
            openModAtIndex(currentModIndex);
            drawFrame();
            startPlaying();
            drawBtnPlay(false);  // fix: always show correct icon after skip
        }

        if (nextHeld) {
            nextHeld = false;
            drawBtnNext(false);
            file.close();
            currentModIndex = (currentModIndex + 1) % modCount;
            openModAtIndex(currentModIndex);
            drawFrame();
            startPlaying();
            drawBtnPlay(false);  // fix: always show correct icon after skip
        }

        if (playHeld) {
            playHeld  = false;
            isPlaying = !isPlaying;
            if (isPlaying) {
                songStart = millis() - pausedElapsed;
                pauseModTrackerState = false;
                tft.fillRect(121, 191, 181, 17, 0x738E);
                drawBoingBall(99, 190, 1 % BOING_FRAME_COUNT);
                tft.setTextSize(2);
                tft.setTextColor(0x4208);
                tft.drawString("Ready: Playing", 123, 192);
            } else {
                pausedElapsed = millis() - songStart;
                pauseModTrackerState = true;
                tft.fillRect(121, 191, 181, 17, 0x738E);
                drawBoingBall(99, 190, 1 % BOING_FRAME_COUNT);
                tft.setTextSize(2);
                tft.setTextColor(0x4208);
                tft.drawString("Ready: Paused", 123, 192);
            }
            drawBtnPlay(false);
        }
    }
}
