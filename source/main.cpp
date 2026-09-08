#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "catalog.h"
#include "http.h"
#include "image.h"
#include "ui.h"
#include "player.h"

// ---- App state -------------------------------------------------------------
// Tubi needs no login, so unlike 3DSfinPlus there is no setup/login/server-
// refresh screen and no separate "player" state: playback is a direct,
// blocking call (see playEntry() below), the same control flow Pluto3DS
// uses, rather than a UI state of its own.

enum AppState {
    STATE_LOADING,
    STATE_HOME,
    STATE_ITEMS,   // a drilled-into level: a series' episodes, or search results
    STATE_ERROR,
};

enum PendingLoad {
    LOAD_NONE,
    LOAD_VOD,       // initial home load: Catalog::vod() + cover art
    LOAD_LIVETV,    // Catalog::channels() + icon art
    LOAD_SEARCH,    // Catalog::search(query), pushed as an item level
    LOAD_EPISODES,  // Catalog::episodes(series), pushed as an item level
};

// The two home-level menus, cycled with L/R.
enum HomeMenu { HOME_VOD, HOME_LIVETV };

// ---- Globals ---------------------------------------------------------------

static Catalog catalog;
static AppState    state   = STATE_LOADING;
static PendingLoad  pending = LOAD_VOD;
static std::string  loadMsg = "Loading Tubi...";
static std::string  errorMsg;

static C3D_RenderTarget *topScreen = nullptr, *botScreen = nullptr;
static UI* ui = nullptr;

// Fetching cover art is one HTTP request + JPEG decode per item; on a large
// shelf list or episode list that adds up, so eager art-fetching is capped
// at this many items per screen. Anything past the cap still shows (and is
// fully playable/selectable) as a colored placeholder card, just without
// artwork -- the same "fast mode" trade-off 3DSfinPlus's library screen
// makes for large libraries. Raise this if hardware/network handles it fine.
static constexpr size_t MAX_COVER_FETCH = 60;

// Home VOD grid (Menu 1): Tubi's home-page shelves (movies + series),
// deduped. Fetched once per session.
static std::vector<Entry>     vodEntries;
static std::vector<C2D_Image> vodCovers;
static std::vector<std::string> vodCoverData;
static int  selVod = 0;

// Live TV guide (Menu 2). Fetched once per session (lazily, the first time
// the user switches into the menu).
static std::vector<Entry>     liveChannels;
static std::vector<C2D_Image> liveCovers;
static std::vector<std::string> liveCoverData;
static int  selLive = 0;
static bool liveLoaded = false;

static HomeMenu homeMenu = HOME_VOD;
static bool homeTouchWasHeld  = false;
static bool itemsTouchWasHeld = false;

// One drilled-into level: a series' episodes, or a page of search results.
// Only ever one level deep from either home tab (there is no season level --
// Catalog::episodes() already returns a flat, S/E-labelled list), so a
// single-slot stack (rather than 3DSfinPlus's arbitrary-depth browseStack)
// would do, but a small vector keeps popLevel()/pushLevel() simple and
// leaves room for "drill into a series found via search" without special-
// casing that path.
struct ItemLevel {
    std::string title;
    std::vector<Entry> items;
    std::vector<std::string> coverData;
    std::vector<C2D_Image>   covers;
    int sel = 0;
};
static std::vector<ItemLevel> browseStack;

static std::string searchQuery;
static Entry        drillSeries; // series Entry to fetch episodes() for

// ---- Cover-art helpers ------------------------------------------------------

static void freeVodCovers() {
    for (auto& im : vodCovers) Image_free(&im);
    vodCovers.clear();
}
static void buildVodTextures() {
    freeVodCovers();
    vodCovers.assign(vodEntries.size(), C2D_Image{});
    for (size_t i = 0; i < vodCoverData.size() && i < vodCovers.size(); i++)
        if (!vodCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(vodCoverData[i].data()),
                vodCoverData[i].size(), &vodCovers[i]);
}
static void fetchVodCovers() {
    vodCoverData.assign(vodEntries.size(), std::string());
    for (size_t i = 0; i < vodEntries.size() && i < MAX_COVER_FETCH; i++) {
        if (vodEntries[i].logo.empty()) continue;
        auto r = get(vodEntries[i].logo);
        if (r.ok()) vodCoverData[i] = r.body;
    }
    buildVodTextures();
}

static void freeLiveCovers() {
    for (auto& im : liveCovers) Image_free(&im);
    liveCovers.clear();
}
static void buildLiveTextures() {
    freeLiveCovers();
    liveCovers.assign(liveChannels.size(), C2D_Image{});
    for (size_t i = 0; i < liveCoverData.size() && i < liveCovers.size(); i++)
        if (!liveCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(liveCoverData[i].data()),
                liveCoverData[i].size(), &liveCovers[i]);
}
static void fetchLiveCovers() {
    liveCoverData.assign(liveChannels.size(), std::string());
    for (size_t i = 0; i < liveChannels.size() && i < MAX_COVER_FETCH; i++) {
        if (liveChannels[i].logo.empty()) continue;
        auto r = get(liveChannels[i].logo);
        if (r.ok()) liveCoverData[i] = r.body;
    }
    buildLiveTextures();
}

// ---- Browse-level helpers ---------------------------------------------------
// Invariant (same as 3DSfinPlus): only the top level (browseStack.back())
// holds live GPU textures.

static void freeLevelCovers(ItemLevel& lv) {
    for (auto& im : lv.covers) Image_free(&im);
    lv.covers.clear();
}
static void buildLevelCovers(ItemLevel& lv) {
    freeLevelCovers(lv);
    lv.covers.assign(lv.items.size(), C2D_Image{});
    for (size_t i = 0; i < lv.coverData.size() && i < lv.covers.size(); i++)
        if (!lv.coverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(lv.coverData[i].data()),
                lv.coverData[i].size(), &lv.covers[i]);
}
static void fetchLevelCovers(ItemLevel& lv) {
    lv.coverData.assign(lv.items.size(), std::string());
    for (size_t i = 0; i < lv.items.size() && i < MAX_COVER_FETCH; i++) {
        if (lv.items[i].logo.empty()) continue;
        auto r = get(lv.items[i].logo);
        if (r.ok()) lv.coverData[i] = r.body;
    }
    buildLevelCovers(lv);
}
static void pushLevel(const std::string& title, std::vector<Entry> items) {
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());
    ItemLevel lv;
    lv.title = title;
    lv.items = std::move(items);
    browseStack.push_back(std::move(lv));
    fetchLevelCovers(browseStack.back());
}
static void popLevel() {
    if (browseStack.empty()) return;
    freeLevelCovers(browseStack.back());
    browseStack.pop_back();
    if (!browseStack.empty()) buildLevelCovers(browseStack.back());
}
static void clearBrowse() {
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());
    browseStack.clear();
}

// ---- Graphics start/stop (also used around playback) -----------------------

static bool graphicsStart() {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); return false; }
    C2D_Prepare();
    topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    botScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    return topScreen && botScreen;
}
static void graphicsStop() {
    if (topScreen) C3D_RenderTargetDetachOutput(topScreen);
    if (botScreen) C3D_RenderTargetDetachOutput(botScreen);
    C2D_Fini();
    C3D_Fini();
    topScreen = botScreen = nullptr;
}

// ---- Playback ---------------------------------------------------------------
// No UI state of its own: resolve the entry, tear the GPU context down,
// block in playerPlay() (with a seek-retry loop and, for live channels, the
// same guide/renewal wiring Pluto3DS uses), then bring the GPU context back
// up and resume wherever the UI loop left off. Mirrors Pluto3DS's main.cpp
// control flow rather than 3DSfinPlus's STATE_PLAYER.

static void playEntry(const Entry& e) {
    Playback data = catalog.resolve(e);
    if (!data.error.empty() || data.url.empty()) {
        errorMsg = !data.error.empty() ? data.error
                 : "Could not get a playable stream for \"" + e.title + "\".";
        state = STATE_ERROR;
        return;
    }

    // Free GPU textures before tearing down the 3D context (playback owns
    // both screens and all of VRAM while it runs).
    freeVodCovers();
    freeLiveCovers();
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());
    delete ui; ui = nullptr;
    graphicsStop();

    bool live = (e.kind == "channel");
    if (live) {
        // No in-player guide overlay in this first cut (playerGuideDraw/
        // Input left unset); only the renewal callback is wired, so a
        // channel whose playlist expires mid-session gets a fresh one
        // instead of just dying.
        playerRenewUrl = [e]() -> std::pair<std::string, std::string> {
            Playback fresh = catalog.resolve(e);
            return { fresh.url, fresh.audioUrl };
        };
    }

    double offset = 0, seek = -1;
    do {
        seek = -1;
        playerPlay(data.url, (long long)(data.duration * 10000000.0),
                   "Tubi3DS", e.title, 0, offset, &seek,
                   data.subtitleVtt, nullptr, "", false,
                   data.audioUrl, data.subtitleDebug);
        if (seek >= 0) offset = seek;
    } while (seek >= 0);

    if (live) playerRenewUrl = {};

    if (!graphicsStart()) {
        errorMsg = "Could not restart graphics after playback.";
        state = STATE_ERROR;
        return;
    }
    ui = new UI(topScreen, botScreen);
    buildVodTextures();
    if (liveLoaded) buildLiveTextures();
    if (!browseStack.empty()) buildLevelCovers(browseStack.back());
}

// ---- Software keyboard helper -----------------------------------------------

static std::string swkbdRead(const char* hint) {
    SwkbdState swkbd;
    char buf[256] = {};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    swkbdInputText(&swkbd, buf, sizeof(buf));
    return std::string(buf);
}

// ---- Main -------------------------------------------------------------------

int main() {
    gfxInitDefault();
    Result httpRes = httpcInit(4 * 1024 * 1024);

    bool newModel = false;
    APT_CheckNew3DS(&newModel);
    // Run at the New 3DS clock (804 MHz) + L2 cache when available. No-op on
    // an Old 3DS, so it's always safe to call.
    osSetSpeedupEnable(true);

    if (!graphicsStart()) { gfxExit(); return 1; }
    ui = new UI(topScreen, botScreen);

    if (!newModel) {
        errorMsg = "New 3DS / New 2DS XL required";
        state = STATE_ERROR;
        pending = LOAD_NONE;
    } else if (R_FAILED(httpRes)) {
        errorMsg = "Network service unavailable";
        state = STATE_ERROR;
        pending = LOAD_NONE;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break;

        // --------------------------------------------------------------
        // Phase 1: execute any pending blocking network operation. The
        // previous frame already rendered the loading screen.
        // --------------------------------------------------------------
        if (pending != LOAD_NONE) {
            switch (pending) {
                case LOAD_VOD:
                    vodEntries = catalog.vod();
                    if (!catalog.error.empty()) {
                        errorMsg = catalog.error;
                        state = STATE_ERROR;
                        break;
                    }
                    selVod = 0;
                    homeMenu = HOME_VOD;
                    fetchVodCovers();
                    state = STATE_HOME;
                    break;

                case LOAD_LIVETV:
                    liveChannels = catalog.channels();
                    if (!catalog.error.empty() && liveChannels.empty()) {
                        errorMsg = catalog.error;
                        state = STATE_ERROR;
                        break;
                    }
                    selLive = 0;
                    liveLoaded = true;
                    fetchLiveCovers();
                    state = STATE_HOME;
                    break;

                case LOAD_SEARCH:
                    pushLevel("Search: " + searchQuery, catalog.search(searchQuery));
                    state = STATE_ITEMS;
                    break;

                case LOAD_EPISODES:
                    pushLevel(drillSeries.title, catalog.episodes(drillSeries));
                    state = STATE_ITEMS;
                    break;

                default: break;
            }
            pending = LOAD_NONE;
        }

        // --------------------------------------------------------------
        // Phase 2: handle input for the current state.
        // --------------------------------------------------------------
        switch (state) {
            case STATE_LOADING:
                break;

            case STATE_HOME: {
                // L/R cycle the two home menus.
                if (kDown & (KEY_L | KEY_R)) {
                    homeMenu = (homeMenu == HOME_VOD) ? HOME_LIVETV : HOME_VOD;
                    if (homeMenu == HOME_LIVETV && !liveLoaded) {
                        loadMsg = "Loading Live TV guide...";
                        pending = LOAD_LIVETV;
                        state   = STATE_LOADING;
                    }
                    break;
                }

                // Touch: edge-detected tap (KEY_TOUCH is a held state, so a
                // prev-frame flag turns it into a single "just tapped" event).
                bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
                int  touchHit = -1;
                bool touchSearch = false;
                if (touching && !homeTouchWasHeld) {
                    touchPosition touch; hidTouchRead(&touch);
                    if (homeMenu == HOME_VOD && touch.px >= UI::BOT_W - 72 && touch.py < 24) {
                        touchSearch = true;
                    } else if (homeMenu == HOME_VOD) {
                        touchHit = UI::hitTestBottomGrid(touch.px, touch.py,
                                                          (int)vodEntries.size(), selVod);
                    } else {
                        touchHit = UI::hitTestLiveList(touch.px, touch.py,
                                                        (int)liveChannels.size(), selLive);
                    }
                }
                homeTouchWasHeld = touching;

                if (homeMenu == HOME_VOD) {
                    if (touchSearch || (kDown & KEY_Y)) {
                        std::string q = swkbdRead("Search Tubi");
                        if (!q.empty()) {
                            searchQuery = q;
                            loadMsg = "Searching \"" + q + "\"...";
                            pending = LOAD_SEARCH;
                            state   = STATE_LOADING;
                        }
                        break;
                    }

                    int n    = (int)vodEntries.size();
                    int cols = UI::BGRID_COLS;
                    if (touchHit >= 0) selVod = touchHit;
                    if (kDown & KEY_RIGHT && selVod < n - 1 && (selVod % cols) != cols - 1) selVod++;
                    if (kDown & KEY_LEFT  && (selVod % cols) != 0)                          selVod--;
                    if (kDown & KEY_DOWN  && selVod + cols < n)                             selVod += cols;
                    if (kDown & KEY_UP    && selVod - cols >= 0)                            selVod -= cols;

                    if (kDown & KEY_A && n > 0) {
                        const Entry& e = vodEntries[selVod];
                        if (e.kind == "series") {
                            drillSeries = e;
                            loadMsg = "Loading \"" + e.title + "\"...";
                            pending = LOAD_EPISODES;
                            state   = STATE_LOADING;
                        } else {
                            playEntry(e);
                        }
                    }
                } else { // HOME_LIVETV
                    int n = (int)liveChannels.size();
                    if (touchHit >= 0) selLive = touchHit;
                    if (kDown & KEY_DOWN && selLive < n - 1) selLive++;
                    if (kDown & KEY_UP   && selLive > 0)     selLive--;
                    if (kDown & KEY_A && n > 0) playEntry(liveChannels[selLive]);
                }
                break;
            }

            case STATE_ITEMS: {
                if (kDown & KEY_B) {
                    popLevel();
                    if (browseStack.empty()) state = STATE_HOME;
                    break;
                }

                ItemLevel& lv = browseStack.back();
                int n    = (int)lv.items.size();
                int cols = UI::ITEM_GRID_COLS;

                bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
                if (touching && !itemsTouchWasHeld) {
                    touchPosition touch; hidTouchRead(&touch);
                    int hit = UI::hitTestBottomGrid(touch.px, touch.py, n, lv.sel);
                    if (hit >= 0) lv.sel = hit;
                }
                itemsTouchWasHeld = touching;

                if (kDown & KEY_RIGHT && lv.sel < n - 1 && (lv.sel % cols) != cols - 1) lv.sel++;
                if (kDown & KEY_LEFT  && (lv.sel % cols) != 0)                          lv.sel--;
                if (kDown & KEY_DOWN  && lv.sel + cols < n)                             lv.sel += cols;
                if (kDown & KEY_UP    && lv.sel - cols >= 0)                            lv.sel -= cols;

                if (kDown & KEY_A && n > 0) {
                    const Entry& e = lv.items[lv.sel];
                    if (e.kind == "series") {
                        drillSeries = e;
                        loadMsg = "Loading \"" + e.title + "\"...";
                        pending = LOAD_EPISODES;
                        state   = STATE_LOADING;
                    } else {
                        playEntry(e);
                    }
                }
                break;
            }

            case STATE_ERROR:
                if (kDown & KEY_B) {
                    // Back out to the home screen. If nothing has loaded
                    // yet at all (e.g. the very first vod() call failed),
                    // retry it instead of showing an empty grid.
                    if (vodEntries.empty()) {
                        loadMsg = "Loading Tubi...";
                        pending = LOAD_VOD;
                        state   = STATE_LOADING;
                    } else {
                        state = STATE_HOME;
                    }
                }
                break;
        }

        // --------------------------------------------------------------
        // Phase 3: render the current state.
        // --------------------------------------------------------------
        ui->beginFrame();

        switch (state) {
            case STATE_LOADING:
                ui->drawLoadingScreen(loadMsg);
                break;

            case STATE_HOME:
                if (homeMenu == HOME_VOD)
                    ui->drawVodGrid(vodEntries, vodCovers, selVod);
                else
                    ui->drawLiveTvGuide(liveChannels, liveCovers, selLive);
                break;

            case STATE_ITEMS: {
                ItemLevel& lv = browseStack.back();
                ui->drawItemGrid(lv.items, lv.covers, lv.sel, lv.title);
                break;
            }

            case STATE_ERROR:
                ui->drawErrorScreen(errorMsg);
                break;
        }

        ui->endFrame();
    }

    freeVodCovers();
    freeLiveCovers();
    clearBrowse();
    delete ui;
    graphicsStop();
    if (R_SUCCEEDED(httpRes)) httpcExit();
    gfxExit();
    return 0;
}
