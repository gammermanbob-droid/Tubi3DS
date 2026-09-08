#include "ui.h"
#include "image.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---- Palette ---------------------------------------------------------------
static constexpr u32 COL_BG        = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 COL_BG_BOT    = C2D_Color32(0x16, 0x16, 0x28, 0xFF);
static constexpr u32 COL_BAR       = C2D_Color32(0x0f, 0x3c, 0x78, 0xFF);
static constexpr u32 COL_SEL       = C2D_Color32(0x1a, 0x6b, 0xb5, 0xFF);
static constexpr u32 COL_ROW_ALT   = C2D_Color32(0x22, 0x22, 0x3a, 0xFF);
static constexpr u32 COL_WHITE     = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 COL_GREY      = C2D_Color32(0xaa, 0xaa, 0xaa, 0xFF);
static constexpr u32 COL_YELLOW    = C2D_Color32(0xFF, 0xd7, 0x00, 0xFF);
static constexpr u32 COL_RED       = C2D_Color32(0xFF, 0x44, 0x44, 0xFF);
// Tubi's confirmed brand color (read from tubitv.com's own <meta
// name="theme-color"> tag), used for the wordmark and other Tubi-branded
// accents in place of a real logo asset.
static constexpr u32 COL_TUBI      = C2D_Color32(0xff, 0x50, 0x1a, 0xFF);

// ---- Helpers ---------------------------------------------------------------

std::string UI::formatDuration(double seconds) {
    if (seconds <= 0) return "";
    long long secs = (long long)seconds;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return std::string(buf);
}

std::string UI::truncate(const std::string& str, size_t maxLen) {
    if (str.size() <= maxLen) return str;
    return str.substr(0, maxLen - 3) + "...";
}

// ---- Construction ----------------------------------------------------------

UI::UI(C3D_RenderTarget* top, C3D_RenderTarget* bot)
    : top_(top), bot_(bot) {
    font_    = C2D_FontLoadSystem(CFG_REGION_USA);
    textBuf_ = C2D_TextBufNew(4096);
}

UI::~UI() {
    C2D_TextBufDelete(textBuf_);
    C2D_FontFree(font_);
}

// ---- Low-level draw helpers ------------------------------------------------

void UI::drawText(const std::string& str, float x, float y, float scale, u32 color) {
    C2D_Text t;
    C2D_TextFontParse(&t, font_, textBuf_, str.c_str());
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void UI::drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth) {
    // Simple character-level truncation based on approximate char width
    float charW   = scale * 11.0f; // rough estimate for system font
    int   maxChars = static_cast<int>(maxWidth / charW);
    drawText(truncate(str, maxChars < 4 ? 4 : (size_t)maxChars), x, y, scale, color);
}

void UI::drawRect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

void UI::drawTopBar(const std::string& title) {
    drawRect(0, 0, TOP_W, 22, COL_BAR);
    drawText("Tubi3DS", 6, 4, 0.50f, COL_TUBI);
    drawText(title,     92, 4, 0.50f, COL_WHITE);
}

void UI::drawBottomHints(const std::string& hints) {
    drawRect(0, BOT_H - 20, BOT_W, 20, COL_BAR);
    drawText(hints, 6, BOT_H - 17, 0.42f, COL_GREY);
}

// No embedded logo asset is available (tubitv.com couldn't be reached from
// either build sandbox this project was built in), so the top screen shows
// simple styled text in Tubi's brand color instead of an image.
void UI::drawTubiWordmark(float centerX, float y, float scale) {
    std::string word = "tubi";
    float charW = scale * 11.0f;
    float w = word.size() * charW;
    drawText(word, centerX - w / 2.0f, y, scale, COL_TUBI);
}

// ---- Frame -----------------------------------------------------------------

void UI::beginFrame() {
    C2D_TextBufClear(textBuf_);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_, COL_BG);
    C2D_TargetClear(bot_, COL_BG_BOT);
}

void UI::endFrame() {
    C3D_FrameEnd(0);
}

// ---- Screens ---------------------------------------------------------------

void UI::drawLoadingScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Loading");
    drawText(msg, 8, 110, 0.55f, COL_WHITE);

    C2D_SceneBegin(bot_);
}

void UI::drawErrorScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Error");
    drawRect(0, 26, TOP_W, TOP_H - 26, COL_BG);

    // Multi-line: split on \n
    float y = 60;
    std::string line;
    for (char c : msg) {
        if (c == '\n') {
            drawText(line, 8, y, 0.52f, COL_RED);
            line.clear();
            y += 26;
        } else {
            line += c;
        }
    }
    if (!line.empty()) drawText(line, 8, y, 0.52f, COL_RED);

    C2D_SceneBegin(bot_);
    drawBottomHints("B: Back   START: Quit");
}

// Draw an image so it fills the whole x/y/w/h box without distortion ("cover"):
// scale uniformly to cover the box, then center-crop the overflow by trimming the
// subtexture's UV rect to the box's aspect ratio. (C2D_DrawImageAt with separate
// sx/sy would instead *stretch* the art, smearing portrait covers into the card.)
static void drawImageCover(const C2D_Image& im, float x, float y, float w, float h) {
    const float iw = (float)im.subtex->width;
    const float ih = (float)im.subtex->height;
    if (iw <= 0 || ih <= 0) return;

    // Work on a copy of the subtexture so we can shrink its UV window.
    Tex3DS_SubTexture st = *im.subtex;
    const float imgA  = iw / ih;
    const float boxA  = w / h;

    if (imgA > boxA) {
        // Source is wider than the box -> keep full height, crop left/right.
        const float keep = boxA / imgA;                  // fraction of width shown
        const float trim = (st.right - st.left) * (1.0f - keep) * 0.5f;
        st.left  += trim;
        st.right -= trim;
        st.width  = (u16)(iw * keep);
    } else {
        // Source is taller than the box -> keep full width, crop top/bottom.
        const float keep = imgA / boxA;                  // fraction of height shown
        const float trim = (st.top - st.bottom) * (1.0f - keep) * 0.5f;  // top > bottom
        st.top    -= trim;
        st.bottom += trim;
        st.height  = (u16)(ih * keep);
    }

    C2D_Image cropped = { im.tex, &st };
    C2D_DrawImageAt(cropped, x, y, 0.0f, nullptr, w / (float)st.width, h / (float)st.height);
}

// Bottom-screen touchable card grid shared by the VOD home grid and the item
// (episodes/search) screen. The visible page is derived from `selected`
// rather than a separately-tracked offset, so drawing and
// hitTestBottomGrid() can never drift apart: whichever card is selected is
// always on-screen, on both.
void UI::drawBottomMirrorGrid(const std::vector<C2D_Image>& covers,
                              const std::vector<std::string>& labels,
                              int count, int selected) {
    if (count <= 0) {
        drawText("(nothing here)", 8, 100, 0.46f, COL_GREY);
        return;
    }

    const int perPage = BGRID_COLS * BGRID_ROWS_V;
    int page = (selected >= 0 ? selected : 0) / perPage;
    int base = page * perPage;

    for (int i = 0; i < perPage; i++) {
        int idx = base + i;
        if (idx >= count) break;

        int   c = i % BGRID_COLS, r = i / BGRID_COLS;
        float x = BGRID_MX + c * (BGRID_CARD_W + BGRID_GAP);
        float y = BGRID_MY + r * BGRID_PITCH_Y;
        bool  sel = (idx == selected);

        if (sel) drawRect(x - 2, y - 2, BGRID_CARD_W + 4, BGRID_CARD_H + 4, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) drawImageCover(covers[idx], x, y, BGRID_CARD_W, BGRID_CARD_H);
        else        drawRect(x, y, BGRID_CARD_W, BGRID_CARD_H, COL_ROW_ALT);

        drawRect(x, y + BGRID_CARD_H - 16, BGRID_CARD_W, 16, C2D_Color32(0, 0, 0, 0xB0));
        if (idx < (int)labels.size())
            drawTextBuf(labels[idx], x + 3, y + BGRID_CARD_H - 14, 0.34f, COL_WHITE, BGRID_CARD_W - 6);
    }

    int totalPages = (count + perPage - 1) / perPage;
    if (totalPages > 1) {
        float barH   = BOT_H - BGRID_MY - 6;
        float thumbH = barH / totalPages;
        float thumbY = BGRID_MY + thumbH * page;
        drawRect(BOT_W - 4, BGRID_MY, 4, barH,   COL_ROW_ALT);
        drawRect(BOT_W - 4, thumbY,   4, thumbH, COL_GREY);
    }
}

// Bottom-screen touchable channel list for the Live TV guide. Same
// selection-derives-the-page rule as drawBottomMirrorGrid.
void UI::drawLiveTvRows(const std::vector<Entry>& channels,
                        const std::vector<C2D_Image>& icons,
                        int selected) {
    if (channels.empty()) {
        drawText("(no channels found)", 8, 100, 0.46f, COL_GREY);
        return;
    }

    int page = (selected >= 0 ? selected : 0) / LIVE_ROWS_V;
    int base = page * LIVE_ROWS_V;

    for (int i = 0; i < LIVE_ROWS_V; i++) {
        int idx = base + i;
        if (idx >= (int)channels.size()) break;

        float y   = LIVE_ROW_Y0 + i * LIVE_ROW_H;
        bool  sel = (idx == selected);
        drawRect(0, y, BOT_W, LIVE_ROW_H - 2,
                 sel ? COL_SEL : (i % 2 == 0 ? COL_BG_BOT : COL_ROW_ALT));

        bool hasImg = (idx < (int)icons.size() && icons[idx].tex != nullptr);
        if (hasImg) drawImageCover(icons[idx], 6, y + 4, 38, 38);
        else        drawRect(6, y + 4, 38, 38, COL_ROW_ALT);

        drawTextBuf(channels[idx].title, 52, y + 3, 0.44f, COL_WHITE, BOT_W - 60);
        const std::string& prog = channels[idx].now;
        drawTextBuf(prog.empty() ? "No program data" : prog,
                    52, y + 23, 0.36f, COL_GREY, BOT_W - 60);
    }

    int totalPages = ((int)channels.size() + LIVE_ROWS_V - 1) / LIVE_ROWS_V;
    if (totalPages > 1) {
        float barH   = LIVE_ROWS_V * LIVE_ROW_H;
        float thumbH = barH / totalPages;
        float thumbY = LIVE_ROW_Y0 + thumbH * page;
        drawRect(BOT_W - 4, LIVE_ROW_Y0, 4, barH,   COL_ROW_ALT);
        drawRect(BOT_W - 4, thumbY,      4, thumbH, COL_GREY);
    }
}

// Tab strip shown on the top screen of all three home menus, matching
// Pluto3DS's TV GUIDE / SHOWS / MOVIES layout.
void UI::drawHomeMenuTabs(HomeMenuTab active) {
    const char* names[3] = {"Live TV", "Shows", "Movies"};
    float y = 24.0f;
    drawRect(0, y, TOP_W, 20, COL_BG);
    float x = 8.0f;
    for (int i = 0; i < 3; i++) {
        u32 col = (i == (int)active) ? COL_YELLOW : COL_GREY;
        std::string label = std::string(i == (int)active ? "> " : "  ") + names[i];
        drawText(label, x, y + 2, 0.40f, col);
        x += 110.0f;
    }
    drawText("L/R", TOP_W - 34, y + 2, 0.38f, COL_GREY);
}

int UI::hitTestBottomGrid(int touchX, int touchY, int count, int selected) {
    if (count <= 0) return -1;
    const int perPage = BGRID_COLS * BGRID_ROWS_V;
    int page = (selected >= 0 ? selected : 0) / perPage;
    int base = page * perPage;

    if (touchX < BGRID_MX || touchY < BGRID_MY) return -1;
    float relX = touchX - BGRID_MX;
    float relY = touchY - BGRID_MY;
    int c = (int)(relX / (BGRID_CARD_W + BGRID_GAP));
    int r = (int)(relY / BGRID_PITCH_Y);
    if (c < 0 || c >= BGRID_COLS || r < 0 || r >= BGRID_ROWS_V) return -1;
    // Reject a tap landing in the gap between cards rather than on one.
    if (relX - c * (BGRID_CARD_W + BGRID_GAP) > BGRID_CARD_W) return -1;
    if (relY - r * BGRID_PITCH_Y > BGRID_CARD_H) return -1;

    int idx = base + r * BGRID_COLS + c;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

int UI::hitTestLiveList(int touchX, int touchY, int count, int selected) {
    if (count <= 0) return -1;
    if (touchX < 0 || touchX >= BOT_W || touchY < LIVE_ROW_Y0) return -1;
    int page = (selected >= 0 ? selected : 0) / LIVE_ROWS_V;
    int base = page * LIVE_ROWS_V;
    int row = (int)((touchY - LIVE_ROW_Y0) / LIVE_ROW_H);
    if (row < 0 || row >= LIVE_ROWS_V) return -1;
    int idx = base + row;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

void UI::drawContentGrid(const std::vector<Entry>& items,
                         const std::vector<C2D_Image>& covers,
                         int selected,
                         HomeMenuTab active,
                         const std::string& title) {
    // Top screen: branding only. The grid itself lives only on the touchable
    // bottom screen (see below) so there is exactly one grid, never a second
    // one at a different column count that a D-Pad press could desync from.
    C2D_SceneBegin(top_);
    drawTopBar(title);
    drawHomeMenuTabs(active);

    drawTubiWordmark(TOP_W / 2.0f, 100.0f, 1.6f);
    drawText("Choose something to watch below", 78, 150, 0.45f, COL_GREY);

    if (items.empty())
        drawText("(nothing loaded)", 8, 216, 0.46f, COL_GREY);

    // Bottom screen: the touchable grid (the top screen has no digitizer,
    // so this is the only way to select something by tapping it). No cover
    // art is fetched for this screen (see main.cpp) to keep load times
    // short on a potentially large shelf list -- every card falls back to
    // a colored placeholder, same as an empty `covers` vector always did.
    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    std::vector<std::string> labels;
    labels.reserve(items.size());
    for (const auto& it : items) labels.push_back(it.title);
    drawBottomMirrorGrid(covers, labels, (int)items.size(), selected);

    // Search button, top-right corner.
    drawRect(BOT_W - 72, 2, 68, 20, COL_BAR);
    drawText("Search", BOT_W - 64, 5, 0.38f, COL_WHITE);

    drawBottomHints("A: Open  Y: Search  X/SELECT: Genre  L/R: Menu");
}

// Live TV guide (Menu 2). Top screen: the highlighted channel's icon, name,
// and current program. Bottom screen: the touchable channel list.
void UI::drawLiveTvGuide(const std::vector<Entry>& channels,
                         const std::vector<C2D_Image>& icons,
                         int selected) {
    C2D_SceneBegin(top_);
    drawTopBar("Live TV Guide");
    drawHomeMenuTabs(TAB_LIVETV);

    if (channels.empty()) {
        drawText("No Live TV channels found.", 12, 110, 0.50f, COL_GREY);
    } else if (selected >= 0 && selected < (int)channels.size()) {
        const auto& ch = channels[selected];
        bool hasImg = selected < (int)icons.size() && icons[selected].tex != nullptr;
        float ix = 12, iy = 50, iw = 120, ih = 120;
        if (hasImg) drawImageCover(icons[selected], ix, iy, iw, ih);
        else        drawRect(ix, iy, iw, ih, COL_ROW_ALT);

        float tx = ix + iw + 18;
        drawTextBuf(ch.title, tx, iy + 6, 0.56f, COL_WHITE, TOP_W - tx - 10);
        drawText("Now playing:", tx, iy + 44, 0.42f, COL_GREY);
        drawTextBuf(ch.now.empty() ? "(no program data)" : ch.now,
                    tx, iy + 66, 0.46f, COL_YELLOW, TOP_W - tx - 10);
        if (!ch.next.empty())
            drawTextBuf("Up next: " + ch.next, tx, iy + 92, 0.38f, COL_GREY, TOP_W - tx - 10);
    }

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawLiveTvRows(channels, icons, selected);
    drawBottomHints(channels.empty() ? "L/R: Menu" : "A: Watch Live   L/R: Menu");
}

void UI::drawItemGrid(const std::vector<Entry>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected,
                      const std::string& title) {
    // The interactive grid lives only on the touchable bottom screen (see
    // below); the top screen just shows the title bar and the wordmark,
    // same treatment as the VOD home menu.
    C2D_SceneBegin(top_);
    drawTopBar(title);
    drawTubiWordmark(TOP_W / 2.0f, 110.0f, 1.1f);

    if (items.empty())
        drawText("(no items found)", (TOP_W - 17 * 0.50f * 11.0f) / 2.0f, 190, 0.50f, COL_GREY);

    // Bottom screen: the touchable grid, with a one-line metadata strip for
    // the selected item beneath it.
    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    {
        std::vector<std::string> labels;
        labels.reserve(items.size());
        for (const auto& it : items) labels.push_back(it.title);
        drawBottomMirrorGrid(covers, labels, (int)items.size(), selected);
    }

    bool seriesSelected = false;
    if (!items.empty() && selected < (int)items.size()) {
        auto& it = items[selected];
        seriesSelected = (it.kind == "series");
        std::string meta = it.genre;
        std::string dur = formatDuration(it.duration);
        if (!dur.empty()) meta += (meta.empty() ? "" : "  ") + dur;
        std::string line = truncate(it.title, 34);
        if (!meta.empty()) line += "  -  " + meta;
        drawTextBuf(line, 8, 196, 0.36f, COL_GREY, BOT_W - 16);
    }

    drawBottomHints(seriesSelected ? "A: Open   B: Back" : "A: Play   B: Back");
}
