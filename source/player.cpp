#include "player.h"
#include "audio.h"
#include "stb_image.h"
#include "http.h"
#include "hls.h"
#include "mp4box.h"
#include "segment_crypto.h"
#include <deque>
#include "aacdec.h"
#include <utility>
#include "image.h"
#include "hud_dpad_left.h"
#include "hud_dpad_right.h"
#include "hud_button_b.h"
#include <3ds.h>
#include <citro2d.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
std::function<void()> playerGuideDraw;
std::function<bool()> playerGuideInput;
std::function<void()> playerGuideCleanup;
std::function<std::pair<std::string,std::string>()> playerRenewUrl;

// ─── Dimensions ──────────────────────────────────────────────────────────────
// VID_W/VID_H are the MAX (output buffer is allocated for these). The actual
// coded resolution is read from the H.264 SPS at runtime — Jellyfin preserves
// aspect ratio under the MaxWidth/MaxHeight caps, so the real frame is often
// shorter than 240 (e.g. 400x224 for 16:9). MVD must be configured with the
// real coded dims or render() silently writes nothing.
static constexpr u32 VID_W = 512, VID_H = 288;
static constexpr u32 FB_W  = 240, FB_H  = 400;

// Actual coded dimensions (updated from SPS; default to the max until parsed).
static u32 g_decW = VID_W, g_decH = VID_H;

// On-screen debug overlay (bottom-screen console + green test paint). Hidden by
// default; toggled during playback by holding X + D-Pad Up. File logging to
// player_debug.txt is independent of this flag. DBG() prints only when enabled.
static bool g_dbg = false;
static C3D_RenderTarget* g_playbackHudTarget = nullptr;
static bool g_hudC3dOk = false, g_hudC2dOk = false;
static bool g_hudFrameAttempted = false, g_hudFrameAccepted = false;
static C2D_Image g_hudLeft{}, g_hudRight{}, g_hudB{};
static C2D_Font g_hudFont = nullptr;
static C2D_TextBuf g_hudTextBuf = nullptr;
#define DBG(...) do { if (g_dbg) printf(__VA_ARGS__); } while (0)

static void releaseHudAssets() {
    Image_free(&g_hudLeft);
    Image_free(&g_hudRight);
    Image_free(&g_hudB);
    if (g_hudTextBuf) C2D_TextBufDelete(g_hudTextBuf);
    if (g_hudFont) C2D_FontFree(g_hudFont);
    g_hudTextBuf = nullptr;
    g_hudFont = nullptr;
}

// ─── Buffer sizes ─────────────────────────────────────────────────────────────
static constexpr u32 TS_SZ  = 188;
static constexpr u32 RD_SZ  = TS_SZ * 128;
static constexpr u32 PES_SZ = 512 * 1024;
static constexpr u32 NAL_SZ = 512 * 1024;

// ─── MPEG-TS helpers ──────────────────────────────────────────────────────────
static inline int  tsPid (const u8* p) { return ((p[1]&0x1F)<<8)|p[2]; }
static inline bool tsPUSI(const u8* p) { return (p[1]&0x40)!=0; }

static const u8* tsPayload(const u8* p, int* outSz) {
    u8 afc = (p[3]>>4)&3;
    if (afc == 2) { *outSz=0; return nullptr; }
    int off = 4;
    if (afc == 3) off += 1 + p[4];
    if (off >= (int)TS_SZ) { *outSz=0; return nullptr; }
    *outSz = TS_SZ - off;
    return p + off;
}

// ─── PAT parser ───────────────────────────────────────────────────────────────
static int parsePAT(const u8* pl, int sz) {
    if (sz < 9) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 8 || t[0] != 0x00) return -1;
    int secLen     = ((t[1]&0x0F)<<8)|t[2];
    int entryBytes = secLen - 9;
    const u8* prg  = t + 8;
    for (int i = 0; i+4 <= entryBytes && i+4 <= rem-8; i += 4) {
        int prog = (prg[i]<<8)|prg[i+1];
        int pid  = ((prg[i+2]&0x1F)<<8)|prg[i+3];
        if (prog != 0) return pid;
    }
    return -1;
}

// ─── PMT parser ───────────────────────────────────────────────────────────────
// Returns the H.264 video PID (-1 if none). If audPid is non-null, also reports
// the first AAC audio PID (0x0F = ADTS, 0x11 = LATM) via *audPid, or -1.
static int parsePMT(const u8* pl, int sz, int* audPid = nullptr) {
    if (audPid) *audPid = -1;
    if (sz < 13) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 12 || t[0] != 0x02) return -1;
    int secLen  = ((t[1]&0x0F)<<8)|t[2];
    int piLen   = ((t[10]&0x0F)<<8)|t[11];
    int esBytes = secLen - 13 - piLen;
    const u8* es = t + 12 + piLen;
    int remEs    = rem - 12 - piLen;
    int vid = -1;
    for (int o = 0; o+5 <= esBytes && o+5 <= remEs; ) {
        int type = es[o];
        int pid  = ((es[o+1]&0x1F)<<8)|es[o+2];
        int esil = ((es[o+3]&0x0F)<<8)|es[o+4];
        if (type == 0x1B && vid < 0) vid = pid;                       // H.264 video
        else if ((type == 0x0F || type == 0x11) && audPid && *audPid < 0)
            *audPid = pid;                                            // AAC audio
        o += 5 + esil;
    }
    return vid;
}

// ─── PES header skip ──────────────────────────────────────────────────────────
static int pesHeaderLen(const u8* pay, int sz) {
    if (sz < 9 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return 0;
    return 9 + pay[8];
}

// Extract the 90 kHz presentation timestamp from a PES header, or -1 if absent.
static long long pesPTS(const u8* pay, int sz) {
    if (sz < 14 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return -1;
    if (!(pay[7] & 0x80)) return -1;             // PTS_DTS_flags: no PTS present
    return ((long long)((pay[9]  >> 1) & 0x07) << 30)
         | ((long long) pay[10]               << 22)
         | ((long long)((pay[11] >> 1) & 0x7F) << 15)
         | ((long long) pay[12]               <<  7)
         | ((long long)((pay[13] >> 1) & 0x7F));
}

// ─── Bottom-screen info block (text console: 30 rows x 40 cols) ──────────────
// The block sits at the top of the screen. Metadata grows downward from
// ROW_META and is at most 6 rows (series + blank + 2 title lines + blank +
// year), so everything below it keeps a fixed row — the bar doesn't shift when
// a title wraps or a movie has no series name. Text spans cols 5..38.
static constexpr int ROW_SUB    = 1;    // bottom-screen subtitles, rows 1..7
static constexpr int ROW_STATUS = 9;    // "Buffering..." / "Seeking..."
static constexpr int ROW_META   = 11;   // series / title / year, rows 11..16
static constexpr int ROW_TIME   = 19;   // "MM:SS / MM:SS"    + "B EXIT"
static constexpr int ROW_BAR    = 21;   // "[####--------]"
static constexpr int ROW_HINTS  = 23;   // "<< -10s  D-PAD  +30s >>"

struct SubtitleCue {
    double start = 0, end = 0;
    std::string text;
};

static double parseVttTime(const std::string& s) {
    int h = 0, m = 0;
    double sec = 0;
    if (sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3)
        return h * 3600.0 + m * 60.0 + sec;
    if (sscanf(s.c_str(), "%d:%lf", &m, &sec) == 2)
        return m * 60.0 + sec;
    return -1;
}

static std::string cleanVttText(const std::string& in) {
    std::string out;
    bool tag = false;
    for (char c : in) {
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; continue; }
        if (!tag) out += c;
    }
    struct Entity { const char* from; const char* to; } entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}
    };
    for (auto& e : entities) {
        size_t p = 0;
        while ((p = out.find(e.from, p)) != std::string::npos)
            out.replace(p, strlen(e.from), e.to);
    }
    return out;
}

static std::vector<SubtitleCue> parseVtt(const std::string& data) {
    std::vector<SubtitleCue> cues;
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(p, e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (e == data.size()) break;
        p = e + 1;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;
        std::string a = lines[i].substr(0, arrow);
        std::string b = lines[i].substr(arrow + 3);
        while (!a.empty() && a.back() == ' ') a.pop_back();
        while (!b.empty() && b.front() == ' ') b.erase(0, 1);
        size_t setting = b.find(' ');
        if (setting != std::string::npos) b.resize(setting);
        SubtitleCue cue;
        cue.start = parseVttTime(a);
        cue.end   = parseVttTime(b);
        for (++i; i < lines.size() && !lines[i].empty(); i++) {
            if (!cue.text.empty()) cue.text += '\n';
            cue.text += cleanVttText(lines[i]);
        }
        if (cue.start >= 0 && cue.end > cue.start && !cue.text.empty())
            cues.push_back(cue);
    }
    return cues;
}

static void drawSubtitle(const std::vector<SubtitleCue>& cues, double posSec) {
    for (int row = ROW_SUB; row < ROW_SUB + 7; row++)
        printf("\x1b[%d;1H                                        ", row);
    const SubtitleCue* active = nullptr;
    for (const auto& cue : cues) {
        if (posSec >= cue.start && posSec < cue.end) { active = &cue; break; }
        if (cue.start > posSec) break;
    }
    if (!active) return;

    std::vector<std::string> rows;
    size_t p = 0;
    while (p < active->text.size() && rows.size() < 3) {
        size_t hard = active->text.find('\n', p);
        size_t end = hard == std::string::npos ? active->text.size() : hard;
        while (p < end && rows.size() < 3) {
            size_t take = end - p;
            if (take > 38) {
                take = 38;
                size_t sp = active->text.rfind(' ', p + take);
                if (sp != std::string::npos && sp > p) take = sp - p;
            }
            rows.push_back(active->text.substr(p, take));
            p += take;
            while (p < end && active->text[p] == ' ') p++;
        }
        p = hard == std::string::npos ? active->text.size() : hard + 1;
    }
    int row = ROW_SUB + (6 - (int)rows.size()) / 2;
    for (const auto& s : rows) {
        int col = 1 + (40 - (int)s.size()) / 2;
        printf("\x1b[%d;%dH%s", row++, col, s.c_str());
    }
}

static void fmtTime(char* buf, size_t n, double sec) {
    if (sec < 0) sec = 0;
    int t = (int)(sec + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0) snprintf(buf, n, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, n, "%02d:%02d", m, s);
}

// Draws "MM:SS / MM:SS" and a [####----] bar under the metadata.
// Uses ANSI cursor positioning so it updates in place (no scrolling).
static void drawSeekBar(double posSec, double durSec) {
    const int barW = 28;
    char bar[barW + 1];
    int filled = 0;
    if (durSec > 0) {
        double frac = posSec / durSec;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        filled = (int)(frac * barW + 0.5);
    }
    for (int i = 0; i < barW; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[barW] = '\0';

    char cur[16];
    fmtTime(cur, sizeof(cur), posSec);
    if (durSec > 0) {
        char tot[16];
        fmtTime(tot, sizeof(tot), durSec);
        printf("\x1b[%d;5H%s / %s    ", ROW_TIME, cur, tot);  // spaces clear leftovers
    } else {
        printf("\x1b[%d;5H%s    ", ROW_TIME, cur);            // unknown duration
    }
    printf("\x1b[%d;5H[%s]", ROW_BAR, bar);
}

// Button hints. Skips go directly under the seek bar (which spans cols 5..34)
// so each label sits on the side it seeks toward; exit goes opposite the
// timecode, past the space drawSeekBar rewrites.
// Static: drawn once alongside drawMeta, redrawn when the debug overlay closes.
static void drawControls() {
    printf("\x1b[%d;22HA PAUSE", ROW_TIME);
    printf("\x1b[%d;31HB EXIT",  ROW_TIME);
    printf("\x1b[%d;5H<< -10s",  ROW_HINTS);
    printf("\x1b[%d;17HD-PAD",   ROW_HINTS);
    printf("\x1b[%d;28H+30s >>", ROW_HINTS);
}

// Series name / episode title / year at the top of the screen, one blank line
// between each item. A long title wraps to a second line instead of being
// truncated. Grows downward from ROW_META; at most 6 rows.
static void drawMeta(const std::string& series, const std::string& title, int year) {
    const size_t W = 34;   // console text width (col 5 .. 38)

    // Wrap the title into up to two lines.
    std::string t1, t2;
    if (title.size() <= W) {
        t1 = title;
    } else {
        size_t len = W;
        size_t sp  = title.rfind(' ', W);
        if (sp != std::string::npos && sp > 0) len = sp;   // break on a space
        t1 = title.substr(0, len);
        size_t j = len;
        while (j < title.size() && title[j] == ' ') j++;
        t2 = title.substr(j);
        if (t2.size() > W) t2 = t2.substr(0, W - 3) + "...";
    }

    int row = ROW_META;

    if (!series.empty()) { printf("\x1b[%d;5H%.34s", row, series.c_str()); row += 2; }
    if (!title.empty()) {
        printf("\x1b[%d;5H%s", row, t1.c_str()); row++;
        if (!t2.empty()) { printf("\x1b[%d;5H%s", row, t2.c_str()); row++; }
        row++;   // blank line after the title
    }
    if (year > 0) printf("\x1b[%d;5H%d", row, year);
}

// ─── Blit 400x240 BGR565 -> 240x400 BGR8 and swap ────────────────────────────
// 3DS top-screen layout: column-major, 240 rows per column.
// Logical (x,y) -> physical offset = (x*240 + (239-y)) * 3
// C3D_Fini is called in main before playerPlay, so gspWaitForVBlank is safe.
// Only swap the top screen to avoid disturbing the console on the bottom screen.
static void blitFrame(const u8* mvdOut, u32 frameCount) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    const u16* src = (const u16*)mvdOut;

    if (frameCount < 3) {
        DBG("fb=%p px[0]=%04X dim=%lux%lu\n",
            (void*)fb, (unsigned)src[0],
            (unsigned long)g_decW, (unsigned long)g_decH);
    }

    // Black out the screen first so a smaller-than-screen frame has no leftover
    // (e.g. the green test paint) in the letterbox margin.
    memset(fb, 0, FB_W * FB_H * 3);

    // Source stride is the coded width (g_decW); clamp to screen bounds.
    u32 maxY = FB_W;   // y maps to screen X (240 wide)
    u32 maxX = FB_H;
    if(g_decW * FB_W > g_decH * FB_H) maxY=g_decH*FB_H/g_decW;
    else maxX=g_decW*FB_W/g_decH;   // x maps to screen Y (400 tall)

    // Centre the picture in the screen so a frame smaller than 400x240 gets equal
    // black bars on both sides instead of hugging the top-left corner. A 4:3
    // source transcodes to 320x240 and otherwise puts all 80px of bar on the right.
    u32 padX = (FB_H - maxX) / 2;                 // horizontal margin (screen 400 across)
    u32 padY = (FB_W - maxY) / 2;                 // vertical margin (screen 240 down)

    for (u32 y = 0; y < maxY; y++) {
        for (u32 x = 0; x < maxX; x++) {
            u16 px  = src[(y*g_decH/maxY) * g_decW + (x*g_decW/maxX)];
            u8  r5  = (px >> 11) & 0x1F;
            u8  g6  = (px >> 5)  & 0x3F;
            u8  b5  =  px        & 0x1F;
            u32 off = ((x + padX) * FB_W + (FB_W - 1 - (y + padY))) * 3;
            fb[off]     = (b5 << 3) | (b5 >> 2);
            fb[off + 1] = (g6 << 2) | (g6 >> 4);
            fb[off + 2] = (r5 << 3) | (r5 >> 2);
        }
    }

    GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
    gspWaitForVBlank();
    gfxScreenSwapBuffers(GFX_TOP, false);
}

// Mirrors to the on-screen console (only when debug is enabled) and always to file.
#define DLOG(dbg, ...) do { if (g_dbg) printf(__VA_ARGS__); if(dbg){fprintf(dbg,__VA_ARGS__);fflush(dbg);} } while(0)

// ─── Decoded-frame FIFO + display scheduler ──────────────────────────────────
// Decode and display are decoupled: MVD writes each frame into a slot of this
// FIFO and the demux keeps running ahead (bounded by the slot count), while
// displayPump() blits each frame only when its presentation time arrives.
//
// The previous design paced inside the decoder, so display, demux and the audio
// feed all stalled together on every pacing sleep. That made A/V sync impossible
// to close: holding video to let audio catch up also halted the demux that
// delivers said audio, so the ndsp queue starved, underrunning in a ~0.5s
// snap/underrun/snap stutter cycle (confirmed on hardware via the sync log).
// With the FIFO, the demux runs up to ~0.7s ahead of the screen, which keeps the
// ndsp queue deep and absorbs the muxer's A/V interleave skew (audio PES for a
// given moment arrive later in the stream than the video PES for that moment).
//
// Frame timing comes from a smooth wall-clock anchor, *disciplined* against
// audio::audioClock() — the program-time PTS of the sample the DSP is playing
// right now, on the same 90 kHz axis as video PTS: small error → anchor slew of
// at most ±2 ms/frame (drift-proof, immune to DSP clock jitter); large error
// (resume start, stall recovery) → one anchor snap, then locked. Frame timing is
// never taken directly from the DSP clock — that was tried and stuttered. All
// anchor math is signed (s64): audio behind video makes intermediate values
// negative, and u64 arithmetic underflowed there.
static constexpr int FIFO_MAX = 16;
struct VidFrame { u8* buf; long long pts; };
static VidFrame  g_fifo[FIFO_MAX];
static int       g_fifoN     = 0;      // slots successfully allocated
static int       g_fifoHead  = 0;      // next frame to display
static int       g_fifoLen   = 0;      // decoded frames waiting
static u32       g_dispCount = 0;      // frames blitted so far
static long long g_lastBlitPts = -1;   // PTS on screen (stale-audio reference)
static long long g_vidFirstPts = -1;   // first video PTS demuxed (stale-audio ref at start)

static long long g_pacePts0  = -1;     // 90 kHz PTS of the anchor frame
static s64       g_paceWall0 = 0;      // osGetTime() (ms) scheduled for the anchor PTS
static FILE*     g_paceLog   = nullptr;   // sync-state log (player_debug.txt), for tuning
static u32       g_paceFrames = 0;

// Drains the separate-audio ring into the ndsp queue. playerPlay() points this
// at a lambda (capturing its own demux locals) whenever hasSeparateAudio, and
// clears it otherwise. displayPump(), just below, invokes it on every wait/nap
// iteration: without this, a paced hold (video slowing to let audio "catch up")
// or a full decode FIFO blocks the only code that was draining g_ringAud, so
// audio can never actually catch up. Declared here (ahead of displayPump, well
// ahead of where playerPlay() assigns it) purely so the compiler can see it.
static std::function<void(FILE*)> g_serviceSepAudio;

static void freeFifoSlots() {
    for (int i = 0; i < g_fifoN; i++) { linearFree(g_fifo[i].buf); g_fifo[i].buf = nullptr; }
    g_fifoN = 0;
}

static void blitHead(FILE* dbg) {
    VidFrame& f = g_fifo[g_fifoHead];
    blitFrame(f.buf, g_dispCount);
    g_dispCount++;
    if (f.pts >= 0) g_lastBlitPts = f.pts;
    g_fifoHead = (g_fifoHead + 1) % g_fifoN;
    g_fifoLen--;
    if (g_dispCount <= 5) DLOG(dbg, "frame %u\n", (unsigned)g_dispCount);
}

// Blit every frame whose presentation time has arrived. waitFree additionally
// blocks (napping in 20ms slices) until a FIFO slot is free for the decoder;
// drain blocks until the FIFO is empty (end of stream). The naps hand the core
// to the download thread, so waiting here never starves the network side.
static void displayPump(bool waitFree, bool drain, bool* stop, FILE* dbg) {
    while (g_fifoLen > 0 && !*stop) {
        long long pts = g_fifo[g_fifoHead].pts;
        if (pts < 0) { blitHead(dbg); continue; }      // unstamped AU: show immediately
        s64 now = (s64)osGetTime();
        if (g_pacePts0 < 0) {                          // first frame anchors the clock
            g_pacePts0 = pts; g_paceWall0 = now;
            blitHead(dbg);
            continue;
        }
        long long dpts = pts - g_pacePts0;
        if (dpts < 0) {
            // A genuine 33-bit PTS wraparound produces a huge negative delta
            // (pts just rolled from near 2^33 back to ~0) — only that case
            // should add a full wrap back. A small negative delta is far more
            // likely a B-frame decoded out of display order, whose PTS is
            // legitimately a bit earlier than the anchor frame's; treating
            // that as a wrap schedules the frame ~26 hours out and every
            // later frame queues up stuck behind it. Clamp those to "due now"
            // instead.
            if (dpts < -(1LL << 32)) dpts += (1LL << 33);
            else dpts = 0;
        }
        s64 target = g_paceWall0 + dpts / 90;          // 90 kHz ticks → ms
        // Diagnostic: log the first few "not anchoring" pacing decisions
        // unconditionally (not sampled/capped like the frame/dec counters) so
        // a stuck-on-frame-1 report shows exactly what target got computed.
        static u32 paceDbgCount = 0;
        if (dbg && paceDbgCount < 10) {
            paceDbgCount++;
            fprintf(dbg, "pace#%u pts=%lld anchorPts=%lld dpts=%lld now=%lld wall0=%lld target=%lld fifoLen=%d\n",
                    (unsigned)paceDbgCount, pts, g_pacePts0, dpts,
                    (long long)now, (long long)g_paceWall0, (long long)target, g_fifoLen);
            fflush(dbg);
        }
        if (now < target) {                            // head frame not due yet
            if (!drain && !(waitFree && g_fifoLen >= g_fifoN)) return;
            // Keep feeding the separate-audio ndsp queue through this wait.
            // Without this, waiting here for the audio clock to "catch up"
            // (the errMs>0 servo branch below) or for a FIFO slot to free
            // starves the very code that pushes new audio to the DSP, so the
            // audio clock can never actually advance — it just falls further
            // behind on every subsequent snap (confirmed on hardware: sync
            // err climbing snap after snap, interleaved with DSP underruns).
            if (g_serviceSepAudio) g_serviceSepAudio(dbg);
            s64 nap = target - now;
            if (nap > 20) nap = 20;
            svcSleepThread(nap * 1000000LL);
            hidScanInput();
            if (hidKeysDown() & KEY_B) { *stop = true; return; }
            continue;
        }

        // Frame is due. Audio-clock servo: err > 0 = video presenting early
        // (audio behind), err < 0 = video late.
        double ac = audio::audioClock();
        s64 errMs = 0;
        if (ac >= 0) {
            errMs = (s64)((pts / 90000.0 - ac) * 1000.0);
            if (errMs > 300 || errMs < -300) {
                if (errMs > 1500) errMs = 1500;        // bound the hold on wild PTS gaps
                g_paceWall0 = now + errMs - dpts / 90;
                if (g_paceLog) { fprintf(g_paceLog, "sync snap err=%lldms\n", (long long)errMs); fflush(g_paceLog); }
                if (errMs > 0) continue;               // target moved ahead → wait again
            } else {
                s64 slew = errMs / 8;                  // heavy damping: absorbs clock jitter
                if (slew >  2) slew =  2;              // imperceptible per frame
                if (slew < -2) slew = -2;
                g_paceWall0 += slew;
            }
        } else if (now - target > 200) {
            // No audio clock (no track, missing dsp_firm, underrun) and we fell
            // well behind: re-anchor instead of racing through the backlog.
            g_pacePts0 = pts; g_paceWall0 = now;
            DBG("pace resync\n");
        }
        if (g_paceLog && (++g_paceFrames % 240) == 0) {
            if (ac >= 0)
                fprintf(g_paceLog, "sync err=%+lldms q=%d drops=%u fifo=%d\n",
                        (long long)errMs, audio::queuedBufs(), audio::droppedBlocks(), g_fifoLen);
            else
                fprintf(g_paceLog, "sync noclock q=%d drops=%u fifo=%d\n",
                        audio::queuedBufs(), audio::droppedBlocks(), g_fifoLen);
            fflush(g_paceLog);
        }
        blitHead(dbg);
    }
}

// ─── Read-ahead download ring buffer ──────────────────────────────────────────
// A background thread continuously pulls the HTTP TS stream into this ring while
// the main thread demuxes/decodes/paces out of it. This decouples network I/O
// from display timing: when paceToPts() sleeps to hold a frame to its real
// presentation time, the download thread keeps the buffer full instead of the
// socket starving — which is what previously caused the stutter (a single thread
// can't both pace display AND keep reading). RING_SZ is a power of two so the
// free-running u32 head/tail counters wrap cleanly; index = pos & RING_MASK.
static constexpr u32 RING_SZ   = 8u * 1024 * 1024;   // ~1.8 min at 0.6 Mbps
static constexpr u32 RING_MASK = RING_SZ - 1;
static constexpr u32 PREBUF_SZ = 512u * 1024;        // fill this much before playing

struct DlRing {
    u8*           data;
    u32           capacity=RING_SZ;  // ring size in bytes (power of two)
    u32           mask=RING_MASK;    // capacity-1; overridden for a smaller
                                      // ring (e.g. the separate-audio ring)
    bool          isAudio=false;     // which half of playerRenewUrl()'s pair
                                      // this ring's dlThread should read
    std::string   playlistUrl;
    double startSec=0;
    FILE*         dbg;
    std::deque<u32> boundaries;
    volatile u32  head;          // producer: total bytes written
    volatile u32  tail;          // consumer: total bytes consumed
    volatile bool producerDone;  // stream ended/errored — no more data coming
    volatile bool consumerStop;  // consumer asked the producer to stop
    LightLock     lock;          // guards the head/tail snapshot
};
static DlRing g_ring;
// Second ring + thread for a channel's separate audio-only playlist (only
// used/allocated when playerPlay() is given a non-empty audioUrl). Smaller
// than g_ring since audio bitrates are a fraction of video's.
static DlRing g_ringAud;

static inline u32 ringUsed(DlRing* r) {
    LightLock_Lock(&r->lock);
    u32 u = r->head - r->tail;   // wrap-safe: both are free-running u32
    LightLock_Unlock(&r->lock);
    return u;
}



static void flushBottomConsole() {
    // Use libctru's framebuffer-aware flush rather than guessing the allocation
    // size/stride. This covers both emulator and hardware framebuffer layouts.
    gfxFlushBuffers();
}

static void hudRect(u8* fb, int x, int y, int w, int h,
                    u8 r, u8 g, u8 b) {
    if (!fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 240) h = 240 - y;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++) {
            u32 off = (px * 240 + (239 - py)) * 3;
            fb[off] = b; fb[off + 1] = g; fb[off + 2] = r;
        }
}

static void drawPlaybackHud(u8* fb, double posSec, double durSec, bool paused) {
    if (!fb) return;

    // High-contrast control deck. Keep it tall so emulator scaling cannot wash
    // the controls into the background at small window sizes.
    hudRect(fb, 0, 118, 320, 122, 20, 20, 20);
    hudRect(fb, 12, 130, 296, 14, 225, 232, 238);
    double frac = durSec > 0 ? posSec / durSec : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    hudRect(fb, 12, 130, (int)(296 * frac), 14, 255, 222, 0);

    // Rewind / forward chevrons.
    for (int i = 0; i < 12; i++) {
        hudRect(fb, 39 + i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 67 + i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 277 - i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 249 - i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
    }

    // Centre play/pause button.
    hudRect(fb, 126, 154, 68, 68, 255, 222, 0);
    if (paused) {
        for (int i = 0; i < 14; i++)
            hudRect(fb, 145 + i, 174 - i / 2, 3, i + 2, 255, 255, 255);
    } else {
        hudRect(fb, 145, 171, 10, 34, 255, 255, 255);
        hudRect(fb, 165, 171, 10, 34, 255, 255, 255);
    }

    // Red B/exit indicator at the far right.
    hudRect(fb, 286, 198, 30, 34, 210, 35, 55);
    hudRect(fb, 293, 205, 15, 5, 255, 255, 255);
    hudRect(fb, 293, 218, 15, 5, 255, 255, 255);
    hudRect(fb, 293, 205, 5, 18, 255, 255, 255);
    hudRect(fb, 304, 209, 5, 10, 255, 255, 255);

    GSPGPU_FlushDataCache(fb, 320 * 240 * 3);
}

static void presentPlaybackBottom(double posSec, double durSec, bool paused,
                                  const std::vector<SubtitleCue>* cues = nullptr) {
    if (g_playbackHudTarget) {
        double frac = durSec > 0 ? posSec / durSec : 0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        g_hudFrameAttempted = true;
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
        g_hudFrameAccepted = true;
        if (g_hudTextBuf) C2D_TextBufClear(g_hudTextBuf);
        C2D_TargetClear(g_playbackHudTarget, C2D_Color32(8, 8, 8, 255));
        C2D_SceneBegin(g_playbackHudTarget);
        if(playerGuideDraw) { playerGuideDraw(); C3D_FrameEnd(0); return; }
        // Lyrics own the upper half; playback controls stay in a compact deck.
        C2D_DrawRectSolid(0, 116, 0, 320, 124, C2D_Color32(20, 20, 20, 255));
        C2D_DrawRectSolid(12, 124, 0, 296, 8, C2D_Color32(50, 50, 50, 255));
        C2D_DrawRectSolid(12, 124, 0, 296 * frac, 8, C2D_Color32(255, 222, 0, 255));

        auto text = [](const char* s, float x, float y, float scale, u32 color) {
            if (!g_hudTextBuf) return;
            C2D_Text t;
            // Use Citro2D's already-initialised default font. Loading a second
            // system-font handle after the main UI works on hardware but returns
            // an unusable handle in Azahar/SweepDSEmu, which made every HUD label
            // (including lyrics) silently disappear while shapes still rendered.
            C2D_TextParse(&t, g_hudTextBuf, s);
            C2D_TextOptimize(&t);
            C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
        };
        std::string lyric;
        if (cues && !cues->empty()) {
            // Prefer the cue whose time range contains the playback clock.  Some
            // Jellyfin lyric providers leave gaps (and a few report a slightly
            // early end time), so keep the most recent line visible through a
            // gap.  Before the first timestamp, show the first line instead of a
            // misleading "no lyrics" placeholder.  This also makes unsynchronised
            // sidecars useful on the small bottom screen.
            const SubtitleCue* chosen = &cues->front();
            for (const auto& cue : *cues) {
                if (cue.start <= posSec) chosen = &cue;
                if (posSec >= cue.start && posSec < cue.end) {
                    chosen = &cue;
                    break;
                }
                if (cue.start > posSec) break;
            }
            lyric = chosen->text;
        }
        if (!lyric.empty()) {
            size_t p = 0;
            int row = 0;
            while (p < lyric.size() && row < 4) {
                size_t end = lyric.find('\n', p);
                if (end == std::string::npos) end = lyric.size();
                while (end - p > 34 && row < 4) {
                    size_t cut = lyric.rfind(' ', p + 34);
                    if (cut == std::string::npos || cut < p) cut = p + 34;
                    std::string line = lyric.substr(p, cut - p);
                    text(line.c_str(), 10, 12 + row++ * 23, 0.46f,
                         C2D_Color32(255,255,255,255));
                    p = cut + (cut < lyric.size() && lyric[cut] == ' ');
                }
                if (row < 4 && end > p) {
                    std::string line = lyric.substr(p, end - p);
                    text(line.c_str(), 10, 12 + row++ * 23, 0.46f,
                         C2D_Color32(255,255,255,255));
                }
                p = end + 1;
            }
        } else {
            text("Pluto3DS", 74, 48, 0.42f,
                 C2D_Color32(255,222,0,255));
        }
        if (durSec > 0 && g_hudLeft.tex)
            C2D_DrawImageAt(g_hudLeft, 22, 143, 0.2f, nullptr, 0.72f, 0.72f);
        if (durSec > 0 && g_hudRight.tex)
            C2D_DrawImageAt(g_hudRight, 236, 143, 0.2f, nullptr, 0.72f, 0.72f);

        C2D_DrawCircleSolid(160, 164, 0.1f, 29, C2D_Color32(255, 222, 0, 255));
        if (paused) {
            C2D_DrawTriangle(152, 150, C2D_Color32(255,255,255,255),
                             152, 178, C2D_Color32(255,255,255,255),
                             175, 164, C2D_Color32(255,255,255,255), 0.2f);
        } else {
            C2D_DrawRectSolid(150, 150, 0.2f, 8, 28, C2D_Color32(255,255,255,255));
            C2D_DrawRectSolid(164, 150, 0.2f, 8, 28, C2D_Color32(255,255,255,255));
        }
        text(paused ? "A  PLAY" : "A  PAUSE", paused ? 137 : 132, 199, 0.40f,
             C2D_Color32(255,222,0,255));
        if (durSec > 0) text("-10s", 30, 198, 0.38f, C2D_Color32(255,222,0,255));
        if (durSec > 0) text("+30s", 244, 198, 0.38f, C2D_Color32(255,222,0,255));
        if (g_hudB.tex)
            C2D_DrawImageAt(g_hudB, 277, 202, 0.2f, nullptr, 0.70f, 0.70f);
        text("BACK", 237, 216, 0.36f, C2D_Color32(255,255,255,255));
        C3D_FrameEnd(0);
        return;
    }
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    // Clear and redraw the entire back buffer ourselves. PrintConsole caches a
    // framebuffer pointer and was clearing the just-presented HUD after swaps.
    hudRect(fb, 0, 0, 320, 240, 8, 8, 8);
    // Use the same pointer for the background and every control. Some emulator
    // backends advance their writable buffer when queried more than once.
    drawPlaybackHud(fb, posSec, durSec, paused);
    flushBottomConsole();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}



static bool ringPut(DlRing* r, const u8* src, u32 size) {
    u32 pos = 0;
    while (pos < size && !r->consumerStop) {
        u32 freeb = r->capacity - ringUsed(r);
        if (freeb == 0) { svcSleepThread(2000000LL); continue; }
        u32 hi = r->head & r->mask;
        u32 n = size - pos;
        if (n > freeb) n = freeb;
        u32 contig = r->capacity - hi;
        if (n > contig) n = contig;
        memcpy(r->data + hi, src + pos, n);
        LightLock_Lock(&r->lock);
        r->head += n;
        LightLock_Unlock(&r->lock);
        pos += n;
    }
    return pos == size;
}

// Azahar buffers each HTTP response before exposing it to the emulated http:C
// service. Fetch finite HLS TS segments and concatenate them into the ring.
static void dlThread(void* arg) {
    DlRing* r = (DlRing*)arg;
    uint64_t last=0; bool haveLast=false; unsigned failures=0;
    std::string keyUrl,keyBytes;
    // Live sessions expire; playerRenewUrl (set by the caller only for live
    // channels) fetches a fresh signed playlist URL. Tried proactively on a
    // timer well inside Pluto's session window, and as a last resort before
    // giving up on repeated fetch failures. A no-op (returns false) when
    // unset, e.g. for on-demand playback, which has no long session to renew.
    uint64_t lastRenew=osGetTime();
    const uint64_t RENEW_MS=8*60*1000;
    auto renew=[&]()->bool{
        if(!playerRenewUrl) return false;
        auto fresh=playerRenewUrl();
        const std::string& chosen = r->isAudio ? fresh.second : fresh.first;
        if(chosen.empty()) return false;
        // Log the new URL (query redacted, same convention as the top-of-
        // playback "URL:" line) rather than just "renewed" -- for live
        // channels this now cycles through the ABR ladder on each retry
        // (see Catalog::resolve's variantAttempt), so seeing which rendition
        // was actually picked is the difference between "still the same
        // dead URL" and "moved to a different one" in the next debug log.
        size_t q = chosen.find('?');
        DLOG(r->dbg,"Session renewed; new playlist URL: %.*s%s\n",
             (int)(q == std::string::npos ? chosen.size() : q), chosen.c_str(),
             q == std::string::npos ? "" : "?<redacted>");
        r->playlistUrl=chosen; keyUrl.clear(); keyBytes.clear(); haveLast=false;
        LightLock_Lock(&r->lock);r->boundaries.push_back(static_cast<u32>(r->head));LightLock_Unlock(&r->lock);
        lastRenew=osGetTime(); failures=0;
        return true;
    };
    while(!r->consumerStop) {
        if(osGetTime()-lastRenew>=RENEW_MS) renew();
        Response response=get(r->playlistUrl);
        if(!response.ok()) {
            DLOG(r->dbg,"playlist HTTP=%lu result=%08lX\n",(unsigned long)response.status,(unsigned long)response.error);
            if(++failures>=3) { if(renew()) continue; break; }
            svcSleepThread(1000000000LL); continue;
        }
        auto playlist=hls::parse(response.body);
        DLOG(r->dbg,"Playlist segments=%u end=%d bytes=%lu\n",(unsigned)playlist.segments.size(),playlist.end,(unsigned long)response.body.size());
        if(playlist.fragmented || !playlist.variants.empty() || response.body.rfind("#EXTM3U",0)!=0) {
            DLOG(r->dbg,"Unsupported playlist container\n"); break;
        }
        // Resolve segment/key URIs against where this playlist fetch
        // actually landed, not r->playlistUrl itself -- get()/fetch()
        // follows redirects internally and the caller never otherwise
        // learns the final URL. Using the pre-redirect URL as the resolve
        // base silently reconstructs every relative/absolute-path
        // reference against the wrong host whenever the playlist fetch
        // redirects (see Catalog::resolve()'s identical fix and its
        // comment for the confirmed live-channel case this pattern came
        // from).
        const std::string& playlistBase = response.finalUrl.empty() ? r->playlistUrl : response.finalUrl;
        bool added=false,failed=false;
        double timeline=0;
        for(const auto& segment:playlist.segments) {
            timeline+=segment.duration;
            if(r->startSec>0 && segment.duration>0 && timeline<=r->startSec) continue;
            if(r->consumerStop) break;
            if(haveLast && segment.sequence<=last) continue;
            auto media=get(hls::resolve(playlistBase,segment.uri));
            if(!media.ok()) { DLOG(r->dbg,"Segment fetch HTTP=%lu result=%08lX scheme=%s\n",(unsigned long)media.status,(unsigned long)media.error,hls::resolve(playlistBase,segment.uri).rfind("https://",0)==0?"https":"other"); failed=true; break; }
            if(segment.method=="AES-128") {
                std::string nextKey=hls::resolve(playlistBase,segment.key);
                if(segment.key.empty()) { failed=true; break; }
                if(nextKey!=keyUrl) {
                    auto key=get(nextKey);
                    if(!key.ok() || key.body.size()!=16) { DLOG(r->dbg,"Key fetch HTTP=%lu result=%08lX bytes=%lu\n",(unsigned long)key.status,(unsigned long)key.error,(unsigned long)key.body.size()); failed=true; break; }
                    keyUrl=nextKey;keyBytes=key.body;
                }
                std::array<uint8_t,16> iv;
                if(!hls::parseIV(segment.iv,segment.sequence,iv) || !hls::decryptSegment(media.body,keyBytes,iv)) {
                    DLOG(r->dbg,"Segment IV/decryption failed\n");keyUrl.clear();keyBytes.clear();failed=true;break;
                }
            } else if(!segment.method.empty() && segment.method!="NONE") {
                DLOG(r->dbg,"Unsupported encryption method: %s\n",segment.method.c_str());
                r->producerDone=true;return;
            }
            if(!hls::validTransportStream(media.body)) { DLOG(r->dbg,"Invalid decrypted TS\n");failed=true;break; }
            if(segment.discontinuity || (haveLast && segment.sequence!=last+1)) {
                LightLock_Lock(&r->lock);r->boundaries.push_back(static_cast<u32>(r->head));LightLock_Unlock(&r->lock);
            }
            if(!ringPut(r,reinterpret_cast<const u8*>(media.body.data()),media.body.size())) break;
            DLOG(r->dbg,"segment seq=%llu bytes=%lu boundary=%d encryption=%s\n",(unsigned long long)segment.sequence,(unsigned long)media.body.size(),segment.discontinuity,segment.method.c_str());
            last=segment.sequence;haveLast=true;added=true;
        }
        if(failed) { if(++failures>=3) { if(!renew()) break; } }
        else failures=0;
        if(playlist.end && !failed) break;
        if(!added || failed) svcSleepThread(1000000000LL);
    }
    r->producerDone=true;
}

// ─── fMP4/CMAF demuxing ────────────────────────────────────────────────────
// Tubi (unlike Pluto TV) can serve fragmented-MP4 (CMAF) HLS media playlists
// (#EXT-X-MAP init segment + moof/mdat fragments) instead of MPEG-TS
// segments. Each fragment is already a self-contained, exactly-sized set of
// access units (no PES-style "wait for the next start code to know where
// this one ends"), so rather than forcing it through DlRing's byte-oriented
// ring + 188-byte-packet consumer loop, a fragmented playlist gets its own
// producer thread (dlThreadFmp4) that fully demuxes each fragment itself
// (mp4::parseFragment + Annex-B/ADTS conversion) and pushes complete,
// ready-to-decode access units onto a small queue that the main loop in
// playerPlay() drains straight into the existing processH264()/processAAC().
//
// Scope: this is the VOD/episode case (a finite playlist that ends with
// #EXT-X-ENDLIST). Tubi's live channels aren't wired up to fMP4 yet --
// playerRenewUrl's periodic-refresh dance and #EXT-X-DISCONTINUITY handling
// have no equivalent here -- and audio must come from its own playlist
// (audioUrl set, same as the existing hasSeparateAudio TS path); a fMP4
// fragment that muxes audio and video together in one track isn't handled.
// See playerPlay()'s fMP4-detection probe for how those cases fall back
// safely to the existing TS path instead of starting a half-configured one.
struct Fmp4Au {
    std::vector<u8> data;
    long long pts90k = -1;
    bool keyframe = false;
};
struct Fmp4Queue {
    std::deque<Fmp4Au> q;
    LightLock lock;
    volatile bool producerDone = false;
    volatile bool consumerStop = false;
};
// Cap how many access units a producer will queue up before applying
// backpressure -- generous enough to smooth over a slow decode without
// letting a fast download thread build up unbounded memory (each queued
// item is at most a couple hundred KB for a video keyframe, far less for
// every other sample).
static constexpr size_t FMP4_QUEUE_MAX = 48;

static void fmp4QueuePush(Fmp4Queue* q, Fmp4Au&& au) {
    while (!q->consumerStop) {
        LightLock_Lock(&q->lock);
        bool full = q->q.size() >= FMP4_QUEUE_MAX;
        if (!full) q->q.push_back(std::move(au));
        LightLock_Unlock(&q->lock);
        if (!full) return;
        svcSleepThread(5000000LL);   // 5ms backpressure nap
    }
}
static bool fmp4QueuePop(Fmp4Queue* q, Fmp4Au& out) {
    LightLock_Lock(&q->lock);
    bool has = !q->q.empty();
    if (has) { out = std::move(q->q.front()); q->q.pop_front(); }
    LightLock_Unlock(&q->lock);
    return has;
}
static size_t fmp4QueueSize(Fmp4Queue* q) {
    LightLock_Lock(&q->lock);
    size_t n = q->q.size();
    LightLock_Unlock(&q->lock);
    return n;
}

struct Fmp4Ctx {
    std::string playlistUrl;
    double startSec = 0;
    FILE* dbg = nullptr;
    Fmp4Queue* queue = nullptr;
    const mp4::InitInfo* init = nullptr;      // owned by playerPlay(), outlives the thread
    const mp4::TrackConfig* track = nullptr;  // the one track (video or audio) this thread demuxes
};

static void dlThreadFmp4(void* arg) {
    Fmp4Ctx* ctx = (Fmp4Ctx*)arg;
    Fmp4Queue* q = ctx->queue;
    const mp4::TrackConfig& cfg = *ctx->track;
    uint64_t last = 0; bool haveLast = false; unsigned failures = 0;
    bool sentConfig = false;   // video only: has SPS/PPS been queued at least once yet
    std::vector<u8> tmp(4096); // scratch for one Annex-B-framed SPS/PPS NAL

    while (!q->consumerStop) {
        Response response = get(ctx->playlistUrl);
        if (!response.ok()) {
            DLOG(ctx->dbg, "fmp4 playlist HTTP=%lu result=%08lX\n",
                 (unsigned long)response.status, (unsigned long)response.error);
            if (++failures >= 3) break;
            svcSleepThread(1000000000LL); continue;
        }
        auto playlist = hls::parse(response.body);
        DLOG(ctx->dbg, "fmp4 playlist segments=%u end=%d bytes=%lu\n",
             (unsigned)playlist.segments.size(), playlist.end, (unsigned long)response.body.size());
        // See the identical comment in dlThread(): resolve against where
        // this fetch actually landed, not ctx->playlistUrl, since
        // get()/fetch() absorbs redirects silently.
        const std::string& playlistBase = response.finalUrl.empty() ? ctx->playlistUrl : response.finalUrl;

        bool added = false, failed = false;
        double timeline = 0;
        for (const auto& segment : playlist.segments) {
            timeline += segment.duration;
            if (ctx->startSec > 0 && segment.duration > 0 && timeline <= ctx->startSec) continue;
            if (q->consumerStop) break;
            if (haveLast && segment.sequence <= last) continue;

            auto media = get(hls::resolve(playlistBase, segment.uri));
            if (!media.ok()) {
                DLOG(ctx->dbg, "fmp4 segment fetch HTTP=%lu result=%08lX\n",
                     (unsigned long)media.status, (unsigned long)media.error);
                failed = true; break;
            }

            mp4::FragmentInfo finfo;
            const u8* fdata = reinterpret_cast<const u8*>(media.body.data());
            size_t fsize = media.body.size();
            if (!mp4::parseFragment(fdata, fsize, *ctx->init, finfo)) {
                DLOG(ctx->dbg, "fmp4 fragment seq=%llu: no moof found (%lu bytes)\n",
                     (unsigned long long)segment.sequence, (unsigned long)fsize);
                last = segment.sequence; haveLast = true; added = true; continue;
            }

            for (auto& ftrack : finfo.tracks) {
                if (ftrack.trackId != cfg.trackId) continue;
                for (auto& s : ftrack.samples) {
                    if (q->consumerStop) break;
                    if (s.size == 0) continue;

                    Fmp4Au au;
                    double ptsSec = (double)s.cts / (double)cfg.timescale;
                    au.pts90k = (long long)(ptsSec * 90000.0 + 0.5);
                    au.keyframe = s.keyframe;

                    if (cfg.isVideo) {
                        if (!sentConfig || s.keyframe) {
                            for (auto& sps : cfg.spsList) {
                                size_t w = mp4::writeAnnexBNal(sps.data(), sps.size(), tmp.data(), tmp.size());
                                au.data.insert(au.data.end(), tmp.data(), tmp.data() + w);
                            }
                            for (auto& pps : cfg.ppsList) {
                                size_t w = mp4::writeAnnexBNal(pps.data(), pps.size(), tmp.data(), tmp.size());
                                au.data.insert(au.data.end(), tmp.data(), tmp.data() + w);
                            }
                            sentConfig = true;
                        }
                        size_t cap = (size_t)s.size * 3 + 256;
                        std::vector<u8> dst(cap);
                        size_t w = mp4::avccToAnnexB(fdata + s.offset, s.size, cfg.nalLengthSize,
                                                     dst.data(), dst.size());
                        if (w == 0) {
                            DLOG(ctx->dbg, "fmp4 video sample seq=%llu: AVCC->AnnexB failed (size=%u)\n",
                                 (unsigned long long)segment.sequence, s.size);
                            continue;
                        }
                        au.data.insert(au.data.end(), dst.data(), dst.data() + w);
                    } else {
                        std::vector<u8> dst((size_t)s.size + 7);
                        size_t w = mp4::wrapAdts(cfg, fdata + s.offset, s.size, dst.data(), dst.size());
                        if (w == 0) {
                            DLOG(ctx->dbg, "fmp4 audio sample seq=%llu: ADTS wrap failed (size=%u)\n",
                                 (unsigned long long)segment.sequence, s.size);
                            continue;
                        }
                        au.data.assign(dst.data(), dst.data() + w);
                    }

                    fmp4QueuePush(q, std::move(au));
                }
            }

            DLOG(ctx->dbg, "fmp4 segment seq=%llu bytes=%lu queued=%lu\n",
                 (unsigned long long)segment.sequence, (unsigned long)fsize,
                 (unsigned long)fmp4QueueSize(q));
            last = segment.sequence; haveLast = true; added = true;
        }

        if (failed) { if (++failures >= 3) break; }
        else failures = 0;
        if (playlist.end && !failed) break;
        if (!added || failed) svcSleepThread(1000000000LL);
    }
    q->producerDone = true;
}


// Consumer side: copy exactly n bytes out of ring r (handling wrap). The caller
// must have already confirmed ringUsed(r) >= n.
static void ringTake(DlRing* r, u8* out, u32 n) {
    u32 ti     = r->tail & r->mask;
    u32 contig = r->capacity - ti;
    if (contig >= n) {
        memcpy(out, r->data + ti, n);
    } else {
        memcpy(out, r->data + ti, contig);
        memcpy(out + contig, r->data, n - contig);
    }
    LightLock_Lock(&r->lock);
    r->tail += n;
    LightLock_Unlock(&r->lock);
}

// Sentinel marker positions: 4 corners of the BGR565 output buffer, using the
// CURRENT coded dimensions (g_decW/g_decH) so they match where MVD writes.
// MVD overwrites these when it actually decodes a frame; if they survive a
// process/render call, no frame was produced. (0x11 per reference impl.)
static inline void mvdSetSentinels(u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    buf[0]              = 0x11;
    buf[g_decW*2 - 1]   = 0x11;
    buf[sz - g_decW*2]  = 0x11;
    buf[sz - 1]         = 0x11;
}
static inline bool mvdSentinelsChanged(const u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    return buf[0]              != 0x11 ||
           buf[g_decW*2 - 1]   != 0x11 ||
           buf[sz - g_decW*2]  != 0x11 ||
           buf[sz - 1]         != 0x11;
}

// ─── Minimal H.264 SPS parser (coded resolution only) ────────────────────────
struct BitReader { const u8* d; u32 nbits; u32 pos; };
static u32 brU1(BitReader* b) {
    if (b->pos >= b->nbits) return 0;
    u32 v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1; b->pos++; return v;
}
static u32 brUn(BitReader* b, int n) { u32 v = 0; while (n-- > 0) v = (v << 1) | brU1(b); return v; }
static u32 brUE(BitReader* b) {
    int z = 0; while (b->pos < b->nbits && brU1(b) == 0 && z < 31) z++;
    return ((1u << z) - 1) + brUn(b, z);
}
static int brSE(BitReader* b) { u32 k = brUE(b); return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1); }

// Parse coded width/height from an SPS NAL (payload starts at the NAL header byte).
static bool parseSPS(const u8* nal, u32 len, u32* outW, u32* outH) {
    u8 rbsp[96]; u32 r = 0;
    for (u32 i = 1; i < len && r < sizeof(rbsp); i++) {     // skip NAL header byte
        if (i >= 2 && nal[i] == 3 && nal[i-1] == 0 && nal[i-2] == 0) continue; // emu prevention
        rbsp[r++] = nal[i];
    }
    BitReader b = { rbsp, r * 8, 0 };
    u32 profile = brUn(&b, 8); brUn(&b, 8); brUn(&b, 8);    // profile / constraints / level
    brUE(&b);                                                // seq_parameter_set_id
    if (profile==100||profile==110||profile==122||profile==244||profile==44||
        profile==83||profile==86||profile==118||profile==128||profile==138||
        profile==139||profile==134||profile==135) {
        u32 chroma = brUE(&b);
        if (chroma == 3) brU1(&b);
        brUE(&b); brUE(&b); brU1(&b);                        // bit depths + qpprime
        if (brU1(&b)) {                                      // scaling matrix present
            int cnt = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++)
                if (brU1(&b)) {                              // list present → skip it
                    int sz = (i < 6) ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next != 0) { int d = brSE(&b); next = (last + d + 256) & 255; }
                        last = (next == 0) ? last : next;
                    }
                }
        }
    }
    brUE(&b);                                                // log2_max_frame_num
    u32 poc = brUE(&b);
    if (poc == 0) brUE(&b);
    else if (poc == 1) {
        brU1(&b); brSE(&b); brSE(&b);
        u32 n = brUE(&b); if(n>256) return false; for (u32 i = 0; i < n; i++) brSE(&b);
    }
    brUE(&b); brU1(&b);                                      // max_num_ref_frames + gaps flag
    u32 wMbs = brUE(&b), hMaps = brUE(&b);
    u32 frameMbsOnly = brU1(&b);
    u32 w = (wMbs + 1) * 16;
    u32 h = (hMaps + 1) * 16 * (2 - frameMbsOnly);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    *outW = w; *outH = h; return true;
}

// ─── Access-unit H.264 decoder ───────────────────────────────────────────────
// Faithful adaptation of Core-2-Extreme/Video_player_for_3DS Util_decoder_mvd_decode,
// for an Annex-B MPEG-TS source instead of FFmpeg/AVCC:
//   1. Rebuild the access unit dropping AUD(9)/filler(12) NALs that MVD chokes on.
//   2. Re-arm the output buffer with MVDSTD_SetConfig() *before every frame*.
//   3. Feed the whole access unit in one mvdstdProcessVideoFrame() call.
//      The first AU is fed twice: pass 1 registers SPS/PPS, pass 2 decodes the IDR.
//   4. The frame is usually written during process(); check sentinels first and
//      only fall back to polling mvdstdRenderVideoFrame() if nothing appeared.
static void processH264(u8* pes, u32 pesLen,
                        u8* feedBuf,
                        MVDSTD_Config* cfg, bool* first,
                        bool* stop, u32* frameCount, FILE* dbg, long long auPts) {
    (void)first;
    static u32 auCount = 0;
    auCount++;
    bool log = (auCount <= 25 || *frameCount < 5);
    u32 bufSz = g_decW * g_decH * 2;

    // Feed ONE NAL per mvdstdProcessVideoFrame call (the reference never lumps
    // SPS/PPS/slice together): SPS(7)/PPS(8)→PARAMSET, VCL slice(1..5)→FRAMEREADY→render.
    u32 pos = 0;
    while (pos + 3 < pesLen && !*stop) {
        bool sc3 = pes[pos]==0 && pes[pos+1]==0 && pes[pos+2]==1;
        bool sc4 = !sc3 && pes[pos]==0 && pes[pos+1]==0 &&
                   pes[pos+2]==0 && pes[pos+3]==1;
        if (!sc3 && !sc4) { pos++; continue; }

        u32 scLen   = sc4 ? 4 : 3;
        u32 payload = pos + scLen;           // first byte after start code
        u32 nalEnd  = pesLen;
        for (u32 s = payload + 1; s + 2 < pesLen; s++) {
            if (pes[s]==0 && pes[s+1]==0 &&
                (pes[s+2]==1 || (s+3<pesLen && pes[s+2]==0 && pes[s+3]==1))) {
                nalEnd = s; break;
            }
        }
        u8  nalType  = pes[payload] & 0x1F;
        u32 nalBytes = nalEnd - payload;     // NAL payload (no start code)
        pos = nalEnd;

        if (nalBytes == 0 || nalType == 9 || nalType == 12) continue;  // skip AUD/filler
        if (3 + nalBytes > NAL_SZ) continue;

        // SPS: parse the real coded resolution and reconfigure MVD if it changed.
        // MVD render() writes nothing unless the config dims match the decoded frame.
        if (nalType == 7) {
            u32 w, h;
            if(!parseSPS(pes + payload, nalBytes, &w, &h)) { DLOG(dbg,"Invalid or unsupported SPS\n"); *stop=true; return; }
            if(w != g_decW || h != g_decH) {
                if(w>VID_W || h>VID_H) { DLOG(dbg,"Resolution exceeds buffers: %lux%lu\n",(unsigned long)w,(unsigned long)h); *stop=true; return; }
                g_decW = w; g_decH = h;
                bufSz  = g_decW * g_decH * 2;
                // Output physaddr is re-pointed at a FIFO slot before every VCL.
                mvdstdGenerateDefaultConfig(cfg, g_decW, g_decH, g_decW, g_decH,
                                            nullptr, nullptr, nullptr);
                DLOG(dbg, "SPS dims=%lux%lu (reconfigured MVD)\n",
                     (unsigned long)g_decW, (unsigned long)g_decH);
            }
        }

        // Build a single Annex-B NAL (00 00 01 + payload) in linear feedBuf
        feedBuf[0]=0; feedBuf[1]=0; feedBuf[2]=1;
        memcpy(feedBuf + 3, pes + payload, nalBytes);
        u32 feedLen = 3 + nalBytes;
        GSPGPU_FlushDataCache(feedBuf, feedLen);

        bool isVCL = (nalType >= 1 && nalType <= 5);

        u8* slot = nullptr;
        if (isVCL) {                         // arm a FIFO slot just before a frame NAL
            // If the FIFO is full, display due frames until a slot frees up —
            // this is where playback speed is regulated now (backpressure).
            if (g_fifoLen >= g_fifoN) displayPump(true, false, stop, dbg);
            if (*stop) return;
            slot = g_fifo[(g_fifoHead + g_fifoLen) % g_fifoN].buf;
            cfg->physaddr_outdata0 = osConvertVirtToPhys(slot);
            cfg->physaddr_outdata1 = osConvertVirtToPhys(slot);
            mvdSetSentinels(slot);
            GSPGPU_FlushDataCache(slot, bufSz);
            MVDSTD_SetConfig(cfg);
        }

        Result rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);
        if (rp == (Result)MVD_STATUS_INCOMPLETEPROCESSING)   // retry once
            rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);

        if (log) DLOG(dbg, "NAL t=%u sz=%u proc=%08lX\n",
                     (unsigned)nalType, (unsigned)nalBytes, (unsigned long)rp);

        if (!isVCL) continue;                // parameter sets / SEI: no frame to render

        // Render the decoded frame into the slot (NULL = no re-SetConfig, patched mvd.c)
        bool got = false;
        Result rr = (Result)MVD_STATUS_BUSY;
        for (int i = 0; i < 256; i++) {
            rr = mvdstdRenderVideoFrame(nullptr, false);
            GSPGPU_InvalidateDataCache(slot, bufSz);
            if (mvdSentinelsChanged(slot)) { got = true; break; }
            if (rr != (Result)MVD_STATUS_BUSY) break;
        }
        if (log) DLOG(dbg, " rend=%08lX got=%d px=%02X%02X\n",
                     (unsigned long)rr, (int)got, slot[0], slot[1]);

        if (got) {
            g_fifo[(g_fifoHead + g_fifoLen) % g_fifoN].pts = auPts;
            g_fifoLen++;
            (*frameCount)++;
            if (*frameCount <= 5) DLOG(dbg, "dec %u\n", (unsigned)*frameCount);
            displayPump(false, false, stop, dbg);   // show whatever is due
        }
        hidScanInput();
        if (hidKeysDown() & KEY_B) { *stop = true; return; }
    }
}

// ─── AAC audio decode (Helix) ─────────────────────────────────────────────────
// The audio elementary stream is AAC-LC in ADTS framing (Jellyfin's AudioCodec=aac
// inside an MPEG-TS). Each completed audio PES holds one or more ADTS frames; we
// locate each syncword, decode it to interleaved PCM16 with Helix, and hand the
// PCM to ndsp tagged with its program-time PTS. The PES header carries the PTS of
// the first ADTS frame in the PES; g_audNextPts walks it forward by each frame's
// duration so every pushed block is tagged, which is what lets audio::audioClock()
// report true playback position for the video pacer's servo.
static HAACDecoder g_aac = nullptr;
static double      g_audNextPts = -1.0;   // PTS (sec) of the next ADTS frame decoded
static bool        g_audioOnly = false;
// A separate-audio channel's audio comes from an independently-fetched
// playlist, not the same mux/demuxer clock as the video — so the staleness
// guard below (written for a Jellyfin transcode resume priming audio from
// before the resume point) doesn't apply to it, and must not drop it.
static bool        g_hasSeparateAudio = false;
// One ADTS frame decodes to at most 2048 samples/channel (1024 LC, doubled by SBR)
// × 2 channels = 4096 interleaved shorts.
static short       g_pcm[2048 * 2];

static void processAAC(unsigned char* buf, int len, FILE* dbg) {
    if (!g_aac || len <= 0) return;
    static u32 frames = 0;
    unsigned char* p = buf;
    int bytesLeft = len;
    while (bytesLeft > 0) {
        int off = AACFindSyncWord(p, bytesLeft);
        if (off < 0) break;                          // no (more) ADTS frames here
        p += off; bytesLeft -= off;

        int err = AACDecode(g_aac, &p, &bytesLeft, g_pcm);
        if (err == ERR_AAC_INDATA_UNDERFLOW) break;  // frame split across PES → drop tail
        if (err) {                                   // corrupt frame → skip a byte, resync
            if (bytesLeft > 0) { p++; bytesLeft--; }
            continue;
        }

        AACFrameInfo fi;
        AACGetLastFrameInfo(g_aac, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0) continue;
        if (!audio::ready()) {
            audio::configure(fi.sampRateOut, fi.nChans);
            if (dbg) { fprintf(dbg, "AAC cfg: %d Hz x%d ch\n", fi.sampRateOut, fi.nChans); fflush(dbg); }
        }
        int spc = fi.outputSamps / fi.nChans;
        // Resumed transcodes prime the mux with audio from before the video's
        // start point (seen ~2.8s of it on hardware); queueing that puts audio
        // seconds behind for the whole session. Skip blocks that predate what is
        // (or will first be) on screen. Reference is the displayed frame once one
        // exists, else the first demuxed video PTS.
        long long vref = (g_lastBlitPts >= 0) ? g_lastBlitPts : g_vidFirstPts;
        bool stale = !g_hasSeparateAudio && g_audNextPts >= 0.0 && vref >= 0 &&
                     g_audNextPts < vref / 90000.0 - 0.5;
        if (!stale)
            audio::push(g_pcm, spc, fi.nChans, g_audNextPts);
        if (g_audNextPts >= 0.0 && fi.sampRateOut > 0)
            g_audNextPts += (double)spc / (double)fi.sampRateOut;
        if (++frames <= 3 && dbg) { fprintf(dbg, "AAC frame %u samps=%d\n", (unsigned)frames, fi.outputSamps); fflush(dbg); }
    }
}

static void blitArtwork(const std::string& data) {
    if (data.empty()) return;
    int aw = 0, ah = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(data.data()), (int)data.size(),
        &aw, &ah, &comp, 4);
    if (!rgba || aw <= 0 || ah <= 0) {
        if (rgba) stbi_image_free(rgba);
        return;
    }

    const int side = 224;
    const int left = (400 - side) / 2;
    const int top  = (240 - side) / 2;
    for (int pass = 0; pass < 2; pass++) {
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        memset(fb, 0, FB_W * FB_H * 3);
        for (int y = 0; y < side; y++) {
            int sy = y * ah / side;
            for (int x = 0; x < side; x++) {
                int sx = x * aw / side;
                const u8* p = rgba + (sy * aw + sx) * 4;
                int dx = left + x, dy = top + y;
                u32 off = (dx * FB_W + (FB_W - 1 - dy)) * 3;
                fb[off] = p[2]; fb[off + 1] = p[1]; fb[off + 2] = p[0];
            }
        }
        GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
        gspWaitForVBlank();
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
    stbi_image_free(rgba);
}

// ─── Player entry point ───────────────────────────────────────────────────────
bool playerPlay(const std::string& url, long long runTimeTicks,
                const std::string& series, const std::string& title, int year,
                double startSec, double* seekOut,
                const std::string& subtitleVtt, bool* finishedOut,
                const std::string& artworkData, bool audioOnly,
                const std::string& audioUrl, const std::string& subtitleDebug) {
    if (seekOut) *seekOut = -1.0;
    if (finishedOut) *finishedOut = false;
    g_audioOnly = audioOnly;
    g_hasSeparateAudio = !audioUrl.empty();
    // C2D_CreateScreenTarget replaced gfx's framebuffer pointers with its own VRAM
    // allocation. After C3D_Fini that VRAM is freed but the pointers stay stale.
    // gfxSetScreenFormat is a no-op when the format hasn't changed, so it doesn't
    // fix the pointers. Full gfxExit+gfxInitDefault gives us fresh linear-memory
    // framebuffers that gfxGetFramebuffer and gfxSwapBuffers can safely use.
    gfxExit();
    gfxInitDefault();

    // Clear the top screen before playback (both buffers). When debug is on it
    // paints solid green as a framebuffer-path test; otherwise plain black.
    {
        u8 gch = g_dbg ? 255 : 0;  // green channel
        for (int pass = 0; pass < 2; pass++) {
            u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
            for (u32 i = 0; i < FB_W * FB_H * 3; i += 3) {
                fb[i]   = 0; fb[i+1] = gch; fb[i+2] = 0;
            }
            GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
            gspWaitForVBlank();
            gfxScreenSwapBuffers(GFX_TOP, false);
        }
    }

    // Parse now so even the very first visible HUD frame contains a lyric line.
    std::vector<SubtitleCue> subtitleCues = parseVtt(subtitleVtt);

    // The HUD is fully redrawn into both alternating buffers below. Explicit
    // double buffering matches what Azahar presents after each VBlank.
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    // Do not call consoleInit here: the HUD owns and redraws both bottom buffers.
    g_hudC3dOk = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    g_hudC2dOk = g_hudC3dOk && C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    if (g_hudC2dOk) {
        C2D_Prepare();
        g_playbackHudTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
        if (g_playbackHudTarget)
            C3D_RenderTargetSetOutput(g_playbackHudTarget, GFX_BOTTOM, GFX_LEFT,
                                      GX_TRANSFER_FLIP_VERT(0) |
                                      GX_TRANSFER_OUT_TILED(0) |
                                      GX_TRANSFER_RAW_COPY(0) |
                                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        Image_loadFromMemory(dpad_left_png, dpad_left_png_len, &g_hudLeft);
        Image_loadFromMemory(dpad_right_png, dpad_right_png_len, &g_hudRight);
        Image_loadFromMemory(button_b_png, button_b_png_len, &g_hudB);
        g_hudFont = C2D_FontLoadSystem(CFG_REGION_USA);
        g_hudTextBuf = C2D_TextBufNew(512);
    }
    g_hudFrameAttempted = false;
    g_hudFrameAccepted = false;
    presentPlaybackBottom(startSec,
                          runTimeTicks > 0 ? runTimeTicks / 10000000.0 : 0.0,
                          false, &subtitleCues);
    // Keep the established Azahar presentation order: these top-screen swaps
    // also make its already-rendered bottom HUD visible without touching the
    // bottom framebuffer while Citro3D owns it.
    if (audioOnly) blitArtwork(artworkData);
    DBG("playerPlay\n");

    FILE* dbg = fopen("sdmc:/3ds/pluto3ds/player_debug.txt", "w");
    if (dbg) {
        u16 bottomW = 0, bottomH = 0;
        gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bottomW, &bottomH);
        fprintf(dbg, "BUILD=pluto3ds-0.3.0 vttBytes=%lu bottomFormat=%d dims=%ux%u c3d=%d c2d=%d target=%p frameTry=%d frameOk=%d\n",
                (unsigned long)subtitleVtt.size(),
                (int)gfxGetScreenFormat(GFX_BOTTOM), bottomW, bottomH,
                (int)g_hudC3dOk, (int)g_hudC2dOk, (void*)g_playbackHudTarget,
                (int)g_hudFrameAttempted, (int)g_hudFrameAccepted);
        size_t query = url.find('?');
        fprintf(dbg, "URL: %.*s%s\n\n",
                (int)(query == std::string::npos ? url.size() : query),
                url.c_str(), query == std::string::npos ? "" : "?<redacted>");
        fflush(dbg);
    }

    // Tubi (unlike Pluto TV) can serve fragmented-MP4/CMAF HLS instead of
    // MPEG-TS -- detected by fetching the playlist once up front (dlThread
    // parses it again itself on its first pass either way; one small extra
    // GET here is a trivial cost against not knowing which producer/consumer
    // path to run before starting). VOD/episodes only for now: a fragmented
    // playlist whose init segment can't be fetched/parsed, or that has no
    // recognizable video track (or no audio track when a separate audioUrl
    // was given), silently falls back to the existing TS path, which already
    // reports this cleanly (see "Unsupported playlist container" handling in
    // dlThread + the zero-frames-displayed check at the end of this
    // function) rather than risking a half-configured fMP4 path.
    bool fragmented = false;
    mp4::InitInfo vFmp4Init, aFmp4Init;
    const mp4::TrackConfig* vFmp4Track = nullptr;
    const mp4::TrackConfig* aFmp4Track = nullptr;
    // Live channels (runTimeTicks<=0 -- see the "zero duration denotes a live
    // channel" comment on the seek keys below) never get this probe at all:
    // fMP4 support is VOD/episodes-only to begin with (see dlThreadFmp4's
    // header comment), and a live manifest URL can be a short-lived/one-shot
    // signed link -- spending it here on a probe fetch instead of on the
    // real download thread's first request has been observed to 404 every
    // subsequent fetch (including after playerRenewUrl's "fresh" URL, which
    // is really just this same resolve-then-probe-then-404 sequence
    // repeating). Skipping the probe entirely for live is both correct scope
    // and removes that risk outright.
    if (runTimeTicks > 0) {
        Response probe = get(url);
        if (probe.ok()) {
            hls::Playlist pl = hls::parse(probe.body);
            if (pl.fragmented && !pl.initSegmentUri.empty()) {
                // Resolve against where the playlist fetch actually landed
                // (probe.finalUrl), not the original url -- confirmed via a
                // real player_debug.txt to be exactly why this probe was
                // failing to detect genuinely-fragmented VOD content: this
                // playlist fetch redirects, the init segment URI is a
                // relative/absolute-path reference, and resolving it
                // against the pre-redirect url silently built a broken
                // init-segment URL, so the fetch below failed, vFmp4Track
                // stayed null, and playback fell back to the TS path (which
                // then immediately hit "Unsupported playlist container" on
                // its own independent parse of the very same fragmented
                // playlist). Same root cause and same fix shape as
                // Catalog::resolve()'s identical bug for live channels.
                Response initResp = get(hls::resolve(probe.finalUrl, pl.initSegmentUri));
                if (initResp.ok() &&
                    mp4::parseInit(reinterpret_cast<const u8*>(initResp.body.data()),
                                   initResp.body.size(), vFmp4Init)) {
                    for (auto& t : vFmp4Init.tracks) if (t.isVideo) { vFmp4Track = &t; break; }
                }
            }
        }
        if (vFmp4Track && !audioUrl.empty()) {
            Response probeA = get(audioUrl);
            if (probeA.ok()) {
                hls::Playlist pla = hls::parse(probeA.body);
                if (pla.fragmented && !pla.initSegmentUri.empty()) {
                    // Same fix as the video probe above.
                    Response initRespA = get(hls::resolve(probeA.finalUrl, pla.initSegmentUri));
                    if (initRespA.ok() &&
                        mp4::parseInit(reinterpret_cast<const u8*>(initRespA.body.data()),
                                       initRespA.body.size(), aFmp4Init)) {
                        for (auto& t : aFmp4Init.tracks) if (t.isAudio) { aFmp4Track = &t; break; }
                    }
                }
            }
        }
        fragmented = vFmp4Track != nullptr && (audioUrl.empty() || aFmp4Track != nullptr);
        if (dbg) {
            fprintf(dbg, "fMP4 detect: fragmented=%d videoTrack=%d audioTrack=%d (audioUrl%s)\n",
                    (int)fragmented, (int)(vFmp4Track != nullptr), (int)(aFmp4Track != nullptr),
                    audioUrl.empty() ? " empty" : " set");
            fflush(dbg);
        }
    } else if (dbg) {
        fprintf(dbg, "fMP4 detect: skipped (live channel)\n");
        fflush(dbg);
    }

    // Linear memory buffers (g_ring.data is the background download ring).
    // Frame FIFO slots are allocated best-effort — each holds one full-size
    // BGR565 frame; fewer slots just means less decode-ahead.
    g_ring.capacity = RING_SZ; g_ring.mask = RING_MASK;
    // fMP4 playback doesn't use the byte-oriented TS ring at all (see
    // dlThreadFmp4 above) -- skip its 8 MiB allocation in that mode.
    g_ring.data = fragmented ? nullptr : (u8*)linearAlloc(RING_SZ);
    u8* pesBuf  = (u8*)linearAlloc(PES_SZ);
    u8* nalBuf  = (u8*)linearAlloc(NAL_SZ);
    u8* audBuf  = (u8*)linearAlloc(PES_SZ);   // audio PES accumulator (ADTS frames)
    g_fifoN = 0;
    for (int i = 0; i < FIFO_MAX; i++) {
        g_fifo[i].buf = (u8*)linearAlloc(VID_W * VID_H * 2);
        if (!g_fifo[i].buf) break;
        g_fifo[i].pts = -1;
        g_fifoN++;
    }

    // A channel's separate-audio ring is much smaller than the video ring:
    // audio bitrates are a fraction of video's, so 1 MiB still holds a deep
    // buffer, and New 3DS RAM is tight enough that doubling RING_SZ for every
    // separate-audio channel isn't worth it.
    bool hasSeparateAudio = !audioUrl.empty();
    if (hasSeparateAudio && !fragmented) {
        g_ringAud.capacity = 1u * 1024 * 1024;
        g_ringAud.mask     = g_ringAud.capacity - 1;
        g_ringAud.data     = (u8*)linearAlloc(g_ringAud.capacity);
    }

    if ((!fragmented && !g_ring.data) || !pesBuf || !nalBuf || !audBuf || g_fifoN < 4 ||
        (hasSeparateAudio && !fragmented && !g_ringAud.data)) {
        printf("alloc failed\n");
        if (dbg) { fprintf(dbg, "alloc failed (fifo=%d)\n", g_fifoN); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(audBuf);
        if (hasSeparateAudio) linearFree(g_ringAud.data);
        freeFifoSlots();
        if (g_playbackHudTarget) C3D_RenderTargetDetachOutput(g_playbackHudTarget);
        releaseHudAssets();
        C2D_Fini(); C3D_Fini(); g_playbackHudTarget = nullptr;
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("Buffers OK (fifo=%d)\n", g_fifoN);

    // Reset coded dims to the max so the first SPS always triggers reconfigure.
    g_decW = VID_W; g_decH = VID_H;

    // Reset the frame-pacing anchor (set on the first displayed frame below),
    // the audio PTS walker (set from the first audio PES header), and FIFO state.
    g_pacePts0 = -1; g_paceWall0 = 0;
    g_audNextPts = -1.0;
    g_paceLog = dbg; g_paceFrames = 0;
    g_fifoHead = 0; g_fifoLen = 0; g_dispCount = 0;
    g_lastBlitPts = -1; g_vidFirstPts = -1;

    // Audio-only streams have no video PID. Do not initialize MVD for them:
    // besides wasting memory, Azahar serializes MVD and Citro3D rendering and
    // consequently drops every music-HUD draw command.
    bool mvdOn = !audioOnly;
    Result mvdRet = 0;
    if (mvdOn)
        mvdRet = mvdstdInit(MVDMODE_VIDEOPROCESSING,
                            MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                            MVD_DEFAULT_WORKBUF_SIZE, nullptr);
    if (mvdOn && R_FAILED(mvdRet)) {
        printf("mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet);
        if (dbg) { fprintf(dbg, "mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(audBuf);
        if (hasSeparateAudio) linearFree(g_ringAud.data);
        freeFifoSlots();
        if (g_playbackHudTarget) C3D_RenderTargetDetachOutput(g_playbackHudTarget);
        releaseHudAssets();
        C2D_Fini(); C3D_Fini(); g_playbackHudTarget = nullptr;
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("MVD OK\n");

    MVDSTD_Config mvdCfg{};
    if (mvdOn) {
        mvdstdGenerateDefaultConfig(&mvdCfg, VID_W, VID_H, VID_W, VID_H,
                                    nullptr, nullptr, nullptr);
        mvdCfg.physaddr_outdata0 = osConvertVirtToPhys(g_fifo[0].buf);
        mvdCfg.physaddr_outdata1 = osConvertVirtToPhys(g_fifo[0].buf);
        MVDSTD_SetConfig(&mvdCfg);
        DLOG(dbg, "fifo[0] virt=%08lX phys=%08lX slots=%d\n",
             (unsigned long)g_fifo[0].buf,
             (unsigned long)mvdCfg.physaddr_outdata0, g_fifoN);
    } else {
        DLOG(dbg, "MVD skipped for audio-only playback\n");
    }

    // Override probe: stock libctru returns -1 (FFFFFFFF) for a NULL config;
    // our vendored/patched mvd.c allows NULL. This single line proves which
    // mvd.c is actually linked into the running binary.
    if (mvdOn) {
        Result nullProbe = mvdstdRenderVideoFrame(nullptr, false);
        DLOG(dbg, "NULLrender probe=%08lX (FFFFFFFF=stock libctru, else=patched mvd.c)\n",
             (unsigned long)nullProbe);
    }

    // ─── Audio: ndsp output + Helix AAC decoder ───────────────────────────────
    // ndsp init fails (and audio stays silent) if dsp_firm wasn't dumped to the
    // SD card; that's non-fatal — video still plays. The decoder is configured
    // lazily from the first decoded frame's real sample rate / channel count.
    bool audioOn = audio::init();
    g_aac = AACInitDecoder();
    DLOG(dbg, "audio: ndsp=%d aacDec=%p\n", (int)audioOn, (void*)g_aac);

    DBG("HLS streaming... B=stop\n");

    // ─── Decode loop ─────────────────────────────────────────────────────────
    int  pmtPid    = -1;
    int  vidPid    = -1;
    int  audPid    = -1;
    // Separate-audio ring's own PAT/PMT state (only advanced when
    // hasSeparateAudio; entirely independent of the audio-in-same-mux state
    // above, which stays unused — and audPid stays -1 — in that mode).
    int  pmtPidS   = -1;
    int  audPidS   = -1;
    u32  pesLen    = 0;
    bool pesActive = false;
    u32  audLen    = 0;
    bool audActive = false;
    bool stop       = false;
    bool mvdFirst   = true;   // first access unit is fed twice (params, then decode)
    u32  frameCount = 0;
    u32  pktCount   = 0;

    bool dbgComboPrev = false;   // edge-detect for the X + D-Pad Up debug toggle
    bool prodDoneLogged = false; // one-time log when the download stream ends
    double seekReq = -1.0;       // seek target (sec) requested via D-Pad, -1 = none
    bool seekPrevL = false, seekPrevR = false;   // edge-detect (held-based: inner
                                 // hidScanInput calls would eat hidKeysDown edges)
    bool paused    = false;      // A toggles: demux, decode, blit and DSP all halt
    bool touchWasHeld = false;
    bool pausePrevA = false;     // edge-detect for A (held-based, as above)
    s64  pausedAt  = 0;          // osGetTime() when the pause began

    // Seek-bar state: position from PES PTS, total from the Jellyfin item.
    double    durSec      = runTimeTicks > 0 ? runTimeTicks / 10000000.0 : 0.0;
    double    posSec      = startSec;      // resume offset; PTS delta is added below
    if (dbg) {
        if (!subtitleDebug.empty()) fprintf(dbg, "%s", subtitleDebug.c_str());
        fprintf(dbg, "subtitle cues=%lu\n", (unsigned long)subtitleCues.size());
        for (size_t i = 0; i < subtitleCues.size() && i < 5; i++)
            fprintf(dbg, "cue[%lu]=%.3f..%.3f text=%.48s\n",
                    (unsigned long)i, subtitleCues[i].start,
                    subtitleCues[i].end, subtitleCues[i].text.c_str());
        fflush(dbg);
    }
    long long firstPts    = -1;          // PTS of the first frame (position origin)
    long long curPesPts   = -1;          // PTS of the access unit now being accumulated
    s64       lastBottomRender = -1000;

    if (!g_dbg) {
        for (int pass = 0; pass < 2; pass++) {
            presentPlaybackBottom(startSec, durSec, false, &subtitleCues);
            gspWaitForVBlank();
        }
    }

    // Start the background download thread filling the ring. Same priority as the
    // main thread so the two round-robin on the core; the main thread yields often
    // (pacing sleeps, vblank waits), letting the producer keep the ring topped up.
    s32 mainPrio = 0x30;
    svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
    Thread dlThr = nullptr;
    Thread dlThrAud = nullptr;
    // fMP4 producer contexts/queues -- local (not globals like g_ring/
    // g_ringAud) so they're automatically torn down with this call frame;
    // dlThreadFmp4 only ever reaches them through the pointers baked into
    // Fmp4Ctx, so nothing else needs to reach them by name.
    Fmp4Queue fmp4VideoQueue, fmp4AudioQueue;
    Fmp4Ctx   fmp4VideoCtx, fmp4AudioCtx;
    if (!fragmented) {
        g_ring.playlistUrl  = url;
        g_ring.startSec = startSec;
        g_ring.dbg          = dbg;
        g_ring.boundaries.clear();
        g_ring.head         = 0;
        g_ring.tail         = 0;
        g_ring.producerDone = false;
        g_ring.consumerStop = false;
        LightLock_Init(&g_ring.lock);
        // core 0 (same as main): equal-priority round-robin, and a shared L1 so the
        // ring memory is trivially coherent between producer and consumer.
        dlThr = threadCreate(dlThread, &g_ring, 32 * 1024, mainPrio, 0, false);
        if (!dlThr) g_ring.producerDone = true;
        if (dbg) { fprintf(dbg, "dlThread=%p prio=%ld\n", (void*)dlThr, (long)mainPrio); fflush(dbg); }

        // Second producer for a channel's separate audio-only playlist, same
        // priority/core as the video download thread above.
        if (hasSeparateAudio) {
            g_ringAud.playlistUrl  = audioUrl;
            g_ringAud.startSec     = startSec;
            g_ringAud.isAudio      = true;
            g_ringAud.dbg          = dbg;
            g_ringAud.boundaries.clear();
            g_ringAud.head         = 0;
            g_ringAud.tail         = 0;
            g_ringAud.producerDone = false;
            g_ringAud.consumerStop = false;
            LightLock_Init(&g_ringAud.lock);
            dlThrAud = threadCreate(dlThread, &g_ringAud, 32 * 1024, mainPrio, 0, false);
            if (!dlThrAud) g_ringAud.producerDone = true;
            if (dbg) { fprintf(dbg, "dlThreadAud=%p\n", (void*)dlThrAud); fflush(dbg); }
        }
    } else {
        LightLock_Init(&fmp4VideoQueue.lock);
        fmp4VideoCtx.playlistUrl = url;
        fmp4VideoCtx.startSec    = startSec;
        fmp4VideoCtx.dbg         = dbg;
        fmp4VideoCtx.queue       = &fmp4VideoQueue;
        fmp4VideoCtx.init        = &vFmp4Init;
        fmp4VideoCtx.track       = vFmp4Track;
        dlThr = threadCreate(dlThreadFmp4, &fmp4VideoCtx, 32 * 1024, mainPrio, 0, false);
        if (!dlThr) fmp4VideoQueue.producerDone = true;
        if (dbg) { fprintf(dbg, "dlThreadFmp4(video)=%p prio=%ld\n", (void*)dlThr, (long)mainPrio); fflush(dbg); }

        if (hasSeparateAudio) {
            LightLock_Init(&fmp4AudioQueue.lock);
            fmp4AudioCtx.playlistUrl = audioUrl;
            fmp4AudioCtx.startSec    = startSec;
            fmp4AudioCtx.dbg         = dbg;
            fmp4AudioCtx.queue       = &fmp4AudioQueue;
            fmp4AudioCtx.init        = &aFmp4Init;
            fmp4AudioCtx.track       = aFmp4Track;
            dlThrAud = threadCreate(dlThreadFmp4, &fmp4AudioCtx, 32 * 1024, mainPrio, 0, false);
            if (!dlThrAud) fmp4AudioQueue.producerDone = true;
            if (dbg) { fprintf(dbg, "dlThreadFmp4(audio)=%p\n", (void*)dlThrAud); fflush(dbg); }
        }
    }

    // "Buffering…" hint at the top of the console so a stall reads as buffering
    // rather than a crash. rebuf tracks whether the hint is currently shown.
    bool rebuf = true;
    if (!g_dbg) printf("\x1b[%d;5HBuffering...   ", ROW_STATUS);

    // Prebuffer: wait until enough is queued (or the stream ended) so a brief
    // network dip after playback starts doesn't immediately underrun. TS uses
    // ring bytes as the readiness signal; fMP4 has no equivalent byte count
    // (each queued item is already a complete access unit) so it waits for a
    // handful of queued AUs instead.
    if (!fragmented) {
        while (!stop && ringUsed(&g_ring) < PREBUF_SZ && !g_ring.producerDone) {
            hidScanInput();
            if (hidKeysDown() & KEY_B) { stop = true; break; }
            svcSleepThread(10000000LL);   // 10ms
        }
    } else {
        const size_t FMP4_PREBUF_AUS = 8;
        while (!stop &&
               fmp4QueueSize(&fmp4VideoQueue) < FMP4_PREBUF_AUS && !fmp4VideoQueue.producerDone) {
            hidScanInput();
            if (hidKeysDown() & KEY_B) { stop = true; break; }
            svcSleepThread(10000000LL);
        }
        if (!stop && hasSeparateAudio) {
            while (!stop &&
                   fmp4QueueSize(&fmp4AudioQueue) < FMP4_PREBUF_AUS && !fmp4AudioQueue.producerDone) {
                hidScanInput();
                if (hidKeysDown() & KEY_B) { stop = true; break; }
                svcSleepThread(10000000LL);
            }
        }
    }

    // A single reusable TS packet scratch (the ring hands out whole packets).
    u8 pkt[TS_SZ];
    // Separate scratch buffer for the audio ring's own TS packets — see the
    // comment above drainSepAudioRing() for why this must NOT share `pkt`.
    u8 pktAud[TS_SZ];

    // Drains whatever's ready in the separate-audio ring into the ndsp queue.
    // Defined once here (instead of inline in the loop below) so displayPump()
    // can also call it — via g_serviceSepAudio — while it's blocked napping,
    // which is what actually fixes the underrun-cycling bug: previously this
    // logic only ran once per outer-loop pass, after the video batch and after
    // any displayPump() calls nested inside processH264() had already returned,
    // so any time spent waiting on video pacing (including the servo's
    // deliberate "hold for audio to catch up") starved this and audio could
    // never close the gap it was being held open for.
    auto drainSepAudioRing = [&]() {
        int processedAud = 0;
        while (!stop && audio::queuedBufs() < 24 &&
               ringUsed(&g_ringAud) >= TS_SZ && processedAud < 256) {
            bool boundaryAud=false;
            LightLock_Lock(&g_ringAud.lock);
            if(!g_ringAud.boundaries.empty() && g_ringAud.boundaries.front()==g_ringAud.tail) {
                g_ringAud.boundaries.pop_front(); boundaryAud=true;
            }
            LightLock_Unlock(&g_ringAud.lock);
            if(boundaryAud) {
                DLOG(dbg,"Applying audio-ring discontinuity\n");
                audio::flushQueue();
                audLen=0; audActive=false; pmtPidS=-1; audPidS=-1; g_audNextPts=-1;
                if(g_aac) AACFreeDecoder(g_aac);
                g_aac=AACInitDecoder();
            }
            ringTake(&g_ringAud, pktAud, TS_SZ);
            processedAud++;
            if (pktAud[0] != 0x47) continue;
            int pid = tsPid(pktAud);
            int psz = 0;
            const u8* pay = tsPayload(pktAud, &psz);
            bool pusi = tsPUSI(pktAud);
            if (pid==0 && pay && pmtPidS==-1) {
                pmtPidS = parsePAT(pay, psz);
            } else if (pmtPidS!=-1 && pid==pmtPidS && pay && audPidS==-1) {
                int foundAudio=-1; parsePMT(pay, psz, &foundAudio);
                if (foundAudio>=0) audPidS=foundAudio;
            } else if (audPidS!=-1 && pid==audPidS && pay) {
                if (pusi) {
                    if (audActive && audLen > 0) processAAC(audBuf, (int)audLen, dbg);
                    long long apts = pesPTS(pay, psz);
                    if (apts >= 0) g_audNextPts = apts / 90000.0;
                    int skip = pesHeaderLen(pay, psz);
                    audLen = 0; audActive = true;
                    int cp = psz - skip;
                    if (cp > 0 && (u32)cp <= PES_SZ) { memcpy(audBuf, pay+skip, cp); audLen = cp; }
                } else if (audActive && audLen+psz <= PES_SZ) {
                    memcpy(audBuf+audLen, pay, psz); audLen += psz;
                }
            }
        }
    };
    // fMP4 counterpart to drainSepAudioRing() above: each queued item is
    // already one complete ADTS-framed AAC sample, so there's no PID/PES
    // accumulation to do -- just pop and decode.
    auto drainFmp4AudioQueue = [&]() {
        int processedAud = 0;
        Fmp4Au au;
        while (!stop && audio::queuedBufs() < 24 && processedAud < 64 &&
               fmp4QueuePop(&fmp4AudioQueue, au)) {
            if (au.pts90k >= 0) g_audNextPts = au.pts90k / 90000.0;
            if (!au.data.empty()) processAAC(au.data.data(), (int)au.data.size(), dbg);
            processedAud++;
        }
    };
    if (hasSeparateAudio && !fragmented) {
        g_serviceSepAudio = [&](FILE*){ drainSepAudioRing(); };
    } else if (hasSeparateAudio && fragmented) {
        g_serviceSepAudio = [&](FILE*){ drainFmp4AudioQueue(); };
    } else {
        g_serviceSepAudio = std::function<void(FILE*)>();
    }

    while (!stop) {
        hidScanInput();
        if(playerGuideInput && playerGuideInput()) { stop=true; break; }
        bool touching=(hidKeysHeld() & KEY_TOUCH)!=0;
        bool tapPause=false,tapBack=false,tapLeft=false,tapRight=false,tapSeek=false;
        double tapSeekFrac=0;
        if(touching && !touchWasHeld && !playerGuideInput) {
            touchPosition touch;hidTouchRead(&touch);
            tapBack=touch.px>=230 && touch.py>=202;
            tapPause=touch.px>=124 && touch.px<=198 && touch.py>=132 && touch.py<212;
            tapLeft=touch.px<90 && touch.py>=140 && touch.py<212;
            tapRight=touch.px>=225 && touch.py>=140 && touch.py<202;
            // Progress bar: a full-width strip covering the deck's top padding
            // plus the 8px bar itself (y 116..131), entirely above y=132 where
            // the play/pause button and rewind/forward chevrons start, so a tap
            // here can never also register as one of those. x maps linearly
            // across the bar's own drawn extent (12..308) and clamps at the
            // ends, so tapping past either edge just seeks to start/end.
            if(touch.py>=116 && touch.py<132) {
                tapSeek=true;
                tapSeekFrac=(touch.px-12.0)/296.0;
                if(tapSeekFrac<0) tapSeekFrac=0;
                if(tapSeekFrac>1) tapSeekFrac=1;
            }
        }
        touchWasHeld=touching;
        if(tapBack){stop=true;break;}

        // Toggle the on-screen debug overlay when X + D-Pad Up are held together.
        bool dbgCombo = (hidKeysHeld() & KEY_X) && (hidKeysHeld() & KEY_DUP);
        if (dbgCombo && !dbgComboPrev) {
            g_dbg = !g_dbg;
            if (g_dbg) printf("[debug ON]\n");
            else {                                     // back to clean view: redraw all
                consoleClear();
                drawMeta(series, title, year);
                drawControls();
                lastBottomRender = -1000;              // force a full HUD redraw
            }
        }
        dbgComboPrev = dbgCombo;

        // Seek: D-Pad Left/Right jump -10s/+30s. The live transcode can't be
        // seeked in-stream, so hand the target back to the caller, which starts
        // a fresh stream there (same path as resume).
        bool skL = !playerGuideInput && ((hidKeysHeld() & KEY_DLEFT) != 0 || tapLeft);
        bool skR = !playerGuideInput && ((hidKeysHeld() & KEY_DRIGHT) != 0 || tapRight);
        // A zero duration denotes a live channel. It has no stable timeline, so
        // never restart it with a bogus StartTimeTicks seek.
        if (durSec > 0 && ((skL && !seekPrevL) || (skR && !seekPrevR))) {
            double t = posSec + ((skR && !seekPrevR) ? 30.0 : -10.0);
            if (durSec > 0 && t > durSec - 10.0) t = durSec - 10.0;
            if (t < 0) t = 0;
            seekReq = t;
        }
        seekPrevL = skL; seekPrevR = skR;
        // Tap-to-seek on the progress bar itself. tapSeek is already a
        // one-shot touch-down edge (computed only when !touchWasHeld above,
        // unlike skL/skR's held-based repeat), so it needs no extra edge
        // tracking here — same durSec>0/live-channel guard as the skip keys.
        if (durSec > 0 && tapSeek && !playerGuideInput) {
            double t = tapSeekFrac * durSec;
            if (t > durSec - 10.0) t = durSec - 10.0;
            if (t < 0) t = 0;
            seekReq = t;
        }
        if (seekReq >= 0) {
            // Same slot as the buffering hint. It stays up through teardown and
            // the caller's restart — the next playerPlay's consoleInit clears it
            // — so the gap between the press and the new stream isn't dead air.
            if (!g_dbg) printf("\x1b[%d;5HSeeking...     ", ROW_STATUS);
            if (dbg) { fprintf(dbg, "seek to %.1fs (from %.1fs)\n", seekReq, posSec); fflush(dbg); }
            break;
        }

        if (hidKeysDown() & KEY_B) { stop = true; break; }

        // Pause/resume on A. Held-based edge detect for the same reason as seek.
        // Pausing simply stops the loop doing any work: no demux, no decode, no
        // blit (so the last frame stays on screen) and the DSP channel is halted.
        // The download thread keeps filling the ring and naps once it is full, so
        // a pause applies HTTP backpressure to the transcode rather than losing data.
        bool aHeld = !playerGuideInput && ((hidKeysHeld() & KEY_A) != 0 || tapPause);
        if (aHeld && !pausePrevA) {
            paused = !paused;
            audio::setPaused(paused);
            if (paused) {
                pausedAt = (s64)osGetTime();
                if (!g_dbg) printf("\x1b[%d;5HPaused         ", ROW_STATUS);
                if (dbg) { fprintf(dbg, "paused at %.1fs\n", posSec); fflush(dbg); }
            } else {
                // Wall time ran on while the stream stood still, so every queued
                // frame would now look late and the servo would snap. Push the
                // pacer's anchor forward by exactly the pause length instead.
                s64 held = (s64)osGetTime() - pausedAt;
                g_paceWall0 += held;
                if (!g_dbg) { printf("\x1b[%d;5H               ", ROW_STATUS); rebuf = false; }
                if (dbg) { fprintf(dbg, "resumed after %lldms\n", (long long)held); fflush(dbg); }
            }
        }
        pausePrevA = aHeld;

        if (paused) {
            s64 pausedNow = (s64)osGetTime();
            if (!g_dbg && pausedNow - lastBottomRender >= 66) {
                presentPlaybackBottom(posSec, durSec, true, &subtitleCues);
                lastBottomRender = pausedNow;
            }
            svcSleepThread(30000000LL);
            continue;
        }

        int processed = 0;
        if (!fragmented) {
        // Drain whole TS packets out of the ring. Decoding a frame paces+blits
        // inside processH264 (it may sleep); meanwhile dlThread keeps refilling the
        // ring. Cap the batch so the debug toggle and seek bar stay responsive.
        int batchLimit = audioOnly ? 8 : 128;
        // Music has no video pacer. Leave the compressed stream in the ring while
        // the DSP queue is comfortably full; unlike the old wait inside
        // processAAC(), this returns to the outer loop every frame so controls,
        // lyrics, and the progress bar remain responsive.
        while (!stop && (!audioOnly || audio::queuedBufs() < 24) &&
               ringUsed(&g_ring) >= TS_SZ &&
               processed < batchLimit) {
            bool boundary=false;
            LightLock_Lock(&g_ring.lock);
            if(!g_ring.boundaries.empty() && g_ring.boundaries.front()==g_ring.tail) {
                g_ring.boundaries.pop_front(); boundary=true;
            }
            LightLock_Unlock(&g_ring.lock);
            if(boundary) {
                DLOG(dbg,"Applying stream discontinuity\n");
                audio::flushQueue();g_fifoHead=0;g_fifoLen=0;
                g_pacePts0=-1;g_paceWall0=0;g_lastBlitPts=-1;g_vidFirstPts=-1;g_audNextPts=-1;
                firstPts=-1;curPesPts=-1;pesLen=0;audLen=0;pesActive=false;audActive=false;
                pmtPid=-1;vidPid=-1;audPid=-1;mvdFirst=true;
                if(g_aac) AACFreeDecoder(g_aac);
                g_aac=AACInitDecoder();
                if(mvdOn) {
                    mvdstdExit();
                    Result reset=mvdstdInit(MVDMODE_VIDEOPROCESSING,MVD_INPUT_H264,MVD_OUTPUT_BGR565,MVD_DEFAULT_WORKBUF_SIZE,nullptr);
                    if(R_FAILED(reset)) { stop=true;break; }
                    MVDSTD_SetConfig(&mvdCfg);
                }
            }
            ringTake(&g_ring, pkt, TS_SZ);
            processed++;
            pktCount++;
            if (pktCount % 500 == 0) {
                DBG("pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
                if (dbg) {
                    fprintf(dbg,"pkts=%u pmtPid=%d vidPid=%d frames=%u used=%u\n",
                            (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount,
                            (unsigned)ringUsed(&g_ring));
                    fflush(dbg);
                }
            }

            if (pkt[0] != 0x47) continue;
            int pid = tsPid(pkt);
            int psz = 0;
            const u8* pay = tsPayload(pkt, &psz);
            bool pusi = tsPUSI(pkt);

            if (pid==0 && pay && pmtPid==-1) {
                pmtPid = parsePAT(pay, psz);
                if (pmtPid!=-1) {
                    DBG("PAT->pmtPid=%d\n", pmtPid);
                    if (dbg) { fprintf(dbg,"PAT pmtPid=%d\n",pmtPid); fflush(dbg); }
                }
            } else if (pmtPid!=-1 && pid==pmtPid && pay &&
                       (vidPid==-1 || (!hasSeparateAudio && audPid==-1))) {
                int foundAudio = -1;
                int foundVideo = parsePMT(pay, psz, &foundAudio);
                if (foundVideo >= 0) vidPid = foundVideo;
                // A channel with a separate audio playlist can still carry a
                // fallback/legacy audio ES muxed into the "video-only" variant
                // (common for player compatibility). Ignore it here so it can
                // never collide with the separate-audio ring below, which
                // shares this same audLen/audActive/audBuf accumulator.
                if (!hasSeparateAudio && foundAudio >= 0) audPid = foundAudio;
                if (vidPid!=-1 || audPid!=-1) {
                    DBG("PMT->vidPid=%d audPid=%d\n", vidPid, audPid);
                    if (dbg) { fprintf(dbg,"PMT vidPid=%d audPid=%d\n",vidPid,audPid); fflush(dbg); }
                }
            } else if (!hasSeparateAudio && audPid!=-1 && pid==audPid && pay) {
                // Audio elementary stream: accumulate a PES, then decode its ADTS
                // frames when the next PES starts (PUSI).
                if (pusi) {
                    if (audActive && audLen > 0)
                        processAAC(audBuf, (int)audLen, dbg);
                    // PES PTS = presentation time of the first ADTS frame starting
                    // here; re-syncs g_audNextPts each PES so per-frame accumulation
                    // error (or dropped/corrupt frames) can't build up.
                    long long apts = pesPTS(pay, psz);
                    if (apts >= 0) {
                        g_audNextPts = apts / 90000.0;
                        // Audio-only music has no video PTS to drive the seek bar.
                        // Anchor it to the first audio PES instead.
                        if (vidPid < 0) {
                            if (firstPts < 0) firstPts = apts;
                            long long d = apts - firstPts;
                            if (d < 0) d += (1LL << 33);
                            posSec = startSec + d / 90000.0;
                        }
                    }
                    int skip = pesHeaderLen(pay, psz);
                    audLen = 0; audActive = true;
                    int cp = psz - skip;
                    if (cp > 0 && (u32)cp <= PES_SZ) { memcpy(audBuf, pay+skip, cp); audLen = cp; }
                } else if (audActive && audLen+psz <= PES_SZ) {
                    memcpy(audBuf+audLen, pay, psz); audLen += psz;
                }
            } else if (vidPid!=-1 && pid==vidPid && pay) {
                if (pusi) {
                    long long pts = pesPTS(pay, psz);
                    if (pts >= 0) {
                        if (firstPts < 0) {
                            firstPts = pts;
                            g_vidFirstPts = pts;
                            // Audio demuxed before the first video PES is stale
                            // resume priming if it runs well behind the video
                            // start — purge it or playback begins seconds
                            // desynced. Harmless no-op on aligned streams.
                            if (g_audNextPts >= 0.0 &&
                                g_audNextPts < pts / 90000.0 - 0.5) {
                                audio::flushQueue();
                                if (dbg) { fprintf(dbg, "flushed stale audio (aud=%.2f vid=%.2f)\n",
                                                   g_audNextPts, pts / 90000.0); fflush(dbg); }
                            }
                        }
                        long long d = pts - firstPts;
                        if (d < 0) d += (1LL << 33);   // 33-bit PTS wraparound
                        posSec = startSec + d / 90000.0;
                    }
                    if (pesActive && pesLen > 0)
                        processH264(pesBuf,pesLen,nalBuf,&mvdCfg,&mvdFirst,&stop,&frameCount,dbg,curPesPts);
                    curPesPts = pts;   // PTS now belongs to the AU starting here
                    int skip = pesHeaderLen(pay,psz);
                    pesLen = 0; pesActive = true;
                    int cp = psz-skip;
                    if (cp>0 && (u32)cp<=PES_SZ) { memcpy(pesBuf,pay+skip,cp); pesLen=cp; }
                } else if (pesActive && pesLen+psz<=PES_SZ) {
                    memcpy(pesBuf+pesLen,pay,psz); pesLen+=psz;
                }
            }
        }
        } else {
            // fMP4: each queued item from dlThreadFmp4 is already a complete,
            // exactly-sized access unit (no PES-style "wait for the next start
            // code to know where this one ends" needed) -- pop ready ones and
            // feed them straight to the existing decoder. Audio (when this
            // playlist has a separate one) plays through drainFmp4AudioQueue()
            // below, same as the TS path's drainSepAudioRing(); a fragmented
            // playlist always has hasSeparateAudio true in practice here (see
            // the fMP4-detection probe above), so no muxed-audio case is
            // handled in this branch.
            int batchLimit = 32;
            Fmp4Au au;
            while (!stop && processed < batchLimit && fmp4QueuePop(&fmp4VideoQueue, au)) {
                processed++;
                pktCount++;
                if (au.pts90k >= 0) {
                    if (firstPts < 0) { firstPts = au.pts90k; g_vidFirstPts = au.pts90k; }
                    long long d = au.pts90k - firstPts;
                    if (d < 0) d = 0;   // fMP4 timestamps are monotonic (no 33-bit MPEG-TS wraparound)
                    posSec = startSec + d / 90000.0;
                }
                if (!au.data.empty())
                    processH264(au.data.data(), (u32)au.data.size(), nalBuf, &mvdCfg,
                               &mvdFirst, &stop, &frameCount, dbg, au.pts90k);
            }
        }

        // Drain the channel's separate audio-only ring the same way, into the
        // same audLen/audActive/audBuf accumulator and processAAC() the video
        // loop above uses for muxed audio — the two are mutually exclusive per
        // playerPlay() call (audPid never resolves on a video-only mux), so
        // sharing that state is safe. Own PAT/PMT (pmtPidS/audPidS) and own
        // discontinuity handling, since this is a second, independent mux.
        if (hasSeparateAudio && !fragmented) drainSepAudioRing();
        else if (hasSeparateAudio && fragmented) drainFmp4AudioQueue();

        // Display any frames that came due (also keeps video moving through
        // network stalls, when the batch loop above has nothing to decode).
        if (vidPid >= 0 || fragmented) displayPump(false, false, &stop, dbg);

        // One-time note when the download stream ends — distinguishes a normal
        // end-of-file from the server silently stopping mid-stream (throttling).
        bool producerDoneNow = fragmented
            ? (fmp4VideoQueue.producerDone && (!hasSeparateAudio || fmp4AudioQueue.producerDone))
            : g_ring.producerDone;
        if (producerDoneNow && !prodDoneLogged) {
            prodDoneLogged = true;
            if (dbg) { fprintf(dbg, "producer done: pkts=%u used=%u\n",
                               (unsigned)pktCount,
                               (unsigned)(fragmented ? fmp4QueueSize(&fmp4VideoQueue) : ringUsed(&g_ring)));
                       fflush(dbg); }
        }

        // Buffering indicator: clear once frames flow again, re-show on underrun.
        if (!g_dbg) {
            if (processed > 0) {
                if (rebuf) { printf("\x1b[%d;5H               ", ROW_STATUS); rebuf = false; }
            } else if (!producerDoneNow &&
                       !(audioOnly && audio::queuedBufs() >= 24) && !rebuf) {
                printf("\x1b[%d;5HBuffering...   ", ROW_STATUS); rebuf = true;
            }
        }

        // With no video PTS, the DSP clock is the authoritative music position.
        if (audioOnly && firstPts >= 0) {
            double clock = audio::audioClock();
            if (clock >= 0) posSec = startSec + clock - firstPts / 90000.0;
        }

        // Fully redraw and present the active bottom buffer at 15 Hz. Partial
        // console updates were correct but Azahar periodically presented the
        // untouched alternate buffer, making the HUD flash and vanish.
        s64 renderNow = (s64)osGetTime();
        if (!g_dbg && renderNow - lastBottomRender >= 66) {
            presentPlaybackBottom(posSec, durSec, paused, &subtitleCues);
            lastBottomRender = renderNow;
        }

        // Stream finished and fully drained → done.
        if (fragmented) {
            bool videoEmpty = fmp4VideoQueue.producerDone && fmp4QueueSize(&fmp4VideoQueue) == 0;
            bool audioEmpty = !hasSeparateAudio ||
                              (fmp4AudioQueue.producerDone && fmp4QueueSize(&fmp4AudioQueue) == 0);
            if (videoEmpty && audioEmpty) break;
        } else if (g_ring.producerDone && ringUsed(&g_ring) < TS_SZ) break;
        // Nothing to do this pass (waiting on the network) → yield briefly.
        if (processed == 0) svcSleepThread(5000000LL);   // 5ms
    }

    // Leaving while paused (B or a seek): un-halt the channel so the next stream
    // isn't silent, and drop what's queued rather than draining it at pace.
    if (paused) { audio::setPaused(false); paused = false; stop = true; }

    // Show whatever is still queued at its proper pace before tearing down —
    // unless the user is seeking away, in which case just drop it.
    if (!stop && seekReq < 0 && audActive && audLen > 0)
        processAAC(audBuf, (int)audLen, dbg);
    if (!stop && seekReq < 0 && (vidPid >= 0 || fragmented))
        displayPump(false, true, &stop, dbg);

    bool streamDrained = fragmented
        ? (fmp4VideoQueue.producerDone && fmp4QueueSize(&fmp4VideoQueue) == 0 &&
           (!hasSeparateAudio || (fmp4AudioQueue.producerDone && fmp4QueueSize(&fmp4AudioQueue) == 0)))
        : (g_ring.producerDone && ringUsed(&g_ring) < TS_SZ);
    if (finishedOut)
        *finishedOut = !stop && seekReq < 0 && streamDrained;

    DBG("End: pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
    g_serviceSepAudio = nullptr;   // don't leave a hook into this frame's locals
    g_paceLog = nullptr;
    if (dbg) {
        if (fragmented)
            fprintf(dbg,"End(fmp4): pkts=%u dec=%u disp=%u drained=%d\n",
                    (unsigned)pktCount, (unsigned)frameCount, (unsigned)g_dispCount, (int)streamDrained);
        else
            fprintf(dbg,"End: pkts=%u pmtPid=%d vidPid=%d dec=%u disp=%u\n",
                    (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount,
                    (unsigned)g_dispCount);
        fflush(dbg);
    }

    if (!stop && seekReq < 0)
        svcSleepThread(2000000000LL); // show stats for 2s before returning

    // Stop the producer. HLS responses are finite, so an in-flight request
    // completes promptly without cancelling a continuous HTTP connection.
    if (fragmented) {
        fmp4VideoQueue.consumerStop = true;
        if (hasSeparateAudio) fmp4AudioQueue.consumerStop = true;
    } else {
        g_ring.consumerStop = true;
    }
    if(dlThr) { threadJoin(dlThr, U64_MAX); threadFree(dlThr); }
    if (hasSeparateAudio) {
        if (!fragmented) g_ringAud.consumerStop = true;
        if(dlThrAud) { threadJoin(dlThrAud, U64_MAX); threadFree(dlThrAud); }
        if (!fragmented) { linearFree(g_ringAud.data); g_ringAud.data = nullptr; }
    }
    if (dbg) fclose(dbg);
    AACFreeDecoder(g_aac); g_aac = nullptr;
    audio::exit();
    if (mvdOn) mvdstdExit();
    linearFree(g_ring.data); linearFree(pesBuf);
    linearFree(nalBuf); linearFree(audBuf);
    freeFifoSlots();
    if (seekOut) *seekOut = seekReq;
    if (g_playbackHudTarget) C3D_RenderTargetDetachOutput(g_playbackHudTarget);
    if(playerGuideCleanup) playerGuideCleanup();
    releaseHudAssets();
    C2D_Fini();
    C3D_Fini();
    g_playbackHudTarget = nullptr;
    // Zero frames ever displayed usually means the demuxer bailed on an
    // unsupported playlist (see "Unsupported playlist container" in
    // player_debug.txt -- e.g. a fragmented-MP4/CMAF HLS stream, which this
    // decoder doesn't parse) rather than a real completed/stopped playback.
    // Surfacing that as failure lets the caller show an error instead of
    // silently landing back on the previous screen.
    return g_dispCount > 0 || (seekOut && *seekOut >= 0);
}
