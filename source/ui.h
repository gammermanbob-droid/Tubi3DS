#pragma once
#include <citro2d.h>
#include <string>
#include <vector>
#include "catalog.h"

class UI {
public:
    static constexpr int VISIBLE_ROWS  = 9;   // rows that fit on top screen
    static constexpr int ROW_HEIGHT    = 24;
    static constexpr int TOP_W         = 400;
    static constexpr int TOP_H         = 240;
    static constexpr int BOT_W         = 320;
    static constexpr int BOT_H         = 240;

    // D-pad navigation step for an item list (episodes / search results).
    // Matches BGRID_COLS below, which is what the touchable mirror grid
    // actually renders, so a D-pad press always lands where it looks like it
    // should.
    static constexpr int ITEM_GRID_COLS = 3;

    // Bottom-screen touchable mirror grid, shared by the VOD home grid and
    // the item (episodes/search-results) screen, so a tap's hit-test always
    // matches what's drawn.
    static constexpr float BGRID_MX      = 8.0f;
    static constexpr float BGRID_MY      = 28.0f;
    static constexpr float BGRID_GAP     = 8.0f;
    static constexpr int   BGRID_COLS    = 3;
    static constexpr int   BGRID_ROWS_V  = 2;
    static constexpr float BGRID_CARD_W  = (BOT_W - 2*BGRID_MX - (BGRID_COLS-1)*BGRID_GAP) / BGRID_COLS;
    static constexpr float BGRID_CARD_H  = 76.0f;
    static constexpr float BGRID_PITCH_Y = BGRID_CARD_H + 10.0f;

    // Live TV guide list (bottom screen): one row per channel.
    static constexpr float LIVE_ROW_Y0  = 26.0f;
    static constexpr float LIVE_ROW_H   = 46.0f;
    static constexpr int   LIVE_ROWS_V  = 4;

    // Home-menu L/R tab strip. Only two menus: Tubi needs no login, so
    // there's nothing to resume and no "Continue Watching" tab.
    enum HomeMenuTab { TAB_VOD = 0, TAB_LIVETV = 1 };

    UI(C3D_RenderTarget* top, C3D_RenderTarget* bot);
    ~UI();

    void beginFrame();
    void endFrame();

    void drawLoadingScreen(const std::string& msg);
    void drawErrorScreen(const std::string& msg);

    // Home VOD grid (Menu 1): movies + series pulled from Tubi's home
    // shelves. The touchable grid lives only on the bottom screen (the top
    // screen has no digitizer); the top screen shows the "tubi" wordmark
    // instead of a second, unsynced grid (no real Tubi logo asset is
    // available, so this is styled text in Tubi's brand color).
    void drawVodGrid(const std::vector<Entry>& items,
                     const std::vector<C2D_Image>& covers,
                     int selected);

    // Live TV guide (Menu 2): top screen shows the highlighted channel's
    // icon, name and current program; bottom screen lists every channel
    // with icon + current program, one row per channel, touchable. Only the
    // program airing right now is shown, no schedule look-ahead.
    void drawLiveTvGuide(const std::vector<Entry>& channels,
                         const std::vector<C2D_Image>& icons,
                         int selected);

    // Tab strip shared by both home menus ("L  Home · Live TV  R").
    void drawHomeMenuTabs(HomeMenuTab active);

    // Touch hit-testing for the bottom-screen mirror grid (see BGRID_* above).
    // The visible page is derived from `selected` (same rule drawVodGrid/
    // drawItemGrid use), so this always matches what's currently drawn.
    // Returns the absolute item index under (touchX, touchY), or -1 if the
    // touch isn't over a card.
    static int hitTestBottomGrid(int touchX, int touchY, int count, int selected);

    // Touch hit-testing for the Live TV guide's channel list (LIVE_ROW_* above).
    static int hitTestLiveList(int touchX, int touchY, int count, int selected);

    // A drilled-into level: a series' episodes, or search results. The
    // touchable grid lives on the bottom screen only, matching drawVodGrid;
    // the top screen shows the level title and the "tubi" wordmark. covers
    // is parallel to items; a null tex falls back to a colored placeholder.
    void drawItemGrid(const std::vector<Entry>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected,
                      const std::string& title);

private:
    C3D_RenderTarget* top_;
    C3D_RenderTarget* bot_;
    C2D_Font          font_;
    C2D_TextBuf       textBuf_;

    void drawText(const std::string& str, float x, float y, float scale, u32 color);
    void drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth);
    void drawRect(float x, float y, float w, float h, u32 color);
    void drawTopBar(const std::string& title);
    void drawBottomHints(const std::string& hints);
    // Draws the "tubi" wordmark centered in a box on the top screen, in
    // place of an embedded logo image (no real Tubi logo asset is available
    // from either build sandbox this project was built in).
    void drawTubiWordmark(float centerX, float y, float scale);
    // Bottom-screen touchable card grid shared by the VOD home grid and the
    // item (episodes/search) screen (geometry: BGRID_* above).
    void drawBottomMirrorGrid(const std::vector<C2D_Image>& covers,
                              const std::vector<std::string>& labels,
                              int count, int selected);
    // Bottom-screen touchable channel list for the Live TV guide (LIVE_ROW_* above).
    void drawLiveTvRows(const std::vector<Entry>& channels,
                        const std::vector<C2D_Image>& icons,
                        int selected);

    static std::string formatDuration(double seconds);
    static std::string truncate(const std::string& s, size_t maxLen);
};
