#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <algorithm>
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
    LOAD_VOD,       // Catalog::vod(), split into Shows/Movies (one shared scrape)
    LOAD_LIVETV,    // Catalog::channels() + icon art
    LOAD_SEARCH,    // Catalog::search(query), pushed as an item level
    LOAD_EPISODES,  // Catalog::episodes(series), pushed as an item level
};

// The three home-level menus, cycled with L/R -- same layout and order as
// Pluto3DS's TV GUIDE / SHOWS / MOVIES tabs.
enum HomeMenu { HOME_LIVETV = 0, HOME_SHOWS = 1, HOME_MOVIES = 2 };

// ---- Globals ---------------------------------------------------------------

static Catalog catalog;
static AppState    state   = STATE_LOADING;
static PendingLoad  pending = LOAD_LIVETV;
static std::string  loadMsg = "Loading guide...";
static std::string  errorMsg;

static C3D_RenderTarget *topScreen = nullptr, *botScreen = nullptr;
static UI* ui = nullptr;

// Cover art is only fetched for the Live TV guide, whose channel count is
// small and bounded. The Shows/Movies grids and any drilled-into level
// (episodes/search results) can be long shelf lists, and fetching one HTTP
// request + JPEG decode per poster there would make those screens slow to
// load for little benefit on a grid you're mostly scanning by title -- so
// those always show colored placeholder cards instead (see UI::drawContentGrid
// / drawItemGrid). This cap only bounds the Live TV guide's icon fetch.
static constexpr size_t MAX_COVER_FETCH = 60;

// Shows/Movies (Menu 2/3): Tubi's home-page shelves, fetched once via a
// single Catalog::vod() call and split by Entry::kind. No cover art.
static std::vector<Entry> showsEntries;
static std::vector<Entry> moviesEntries;
static int  selShows  = 0;
static int  selMovies = 0;
static bool vodLoaded = false;

// Genre/category filter, one per tab, matching Pluto3DS's "All genres" +
// unique-genres-seen list. showsFiltered/moviesFiltered are what's actually
// browsed and drawn; they're rebuilt from the full entries list whenever the
// genre selection changes (see rebuildGenreFilter()), not every frame.
static std::vector<std::string> showsGenres  = {"All genres"};
static std::vector<std::string> moviesGenres = {"All genres"};
static int selShowGenre  = 0;
static int selMovieGenre = 0;
static std::vector<Entry> showsFiltered;
static std::vector<Entry> moviesFiltered;

// Live TV guide (Menu 1). Fetched once per session (lazily, the first time
// the user switches into the menu) -- the only screen with real cover art.
static std::vector<Entry>       liveChannels;
static std::vector<C2D_Image>   liveCovers;
static std::vector<std::string> liveCoverData;
static int  selLive = 0;
static bool liveLoaded = false;

static HomeMenu homeMenu = HOME_LIVETV;
static bool homeTouchWasHeld  = false;
static bool itemsTouchWasHeld = false;

// One drilled-into level: a series' episodes, or a page of search results.
// No cover art here either (see MAX_COVER_FETCH comment above).
struct ItemLevel {
    std::string title;
    std::vector<Entry> items;
    int sel = 0;
};
static std::vector<ItemLevel> browseStack;

static std::string searchQuery;
static Entry        drillSeries; // series Entry to fetch episodes() for

// ---- Cover-art helpers (Live TV guide only) ---------------------------------

static void freeCovers(std::vector<C2D_Image>& covers) {
    for (auto& im : covers) Image_free(&im);
    covers.clear();
}
static void buildCoverTextures(std::vector<C2D_Image>& covers,
                               const std::vector<std::string>& coverData,
                               size_t count) {
    freeCovers(covers);
    covers.assign(count, C2D_Image{});
    for (size_t i = 0; i < coverData.size() && i < covers.size(); i++)
        if (!coverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(coverData[i].data()),
                coverData[i].size(), &covers[i]);
}
static void fetchCoverData(std::vector<std::string>& coverData,
                           const std::vector<Entry>& entries) {
    coverData.assign(entries.size(), std::string());
    for (size_t i = 0; i < entries.size() && i < MAX_COVER_FETCH; i++) {
        if (entries[i].logo.empty()) continue;
        auto r = get(entries[i].logo);
        if (r.ok()) coverData[i] = r.body;
    }
}
static void fetchLiveCovers() {
    fetchCoverData(liveCoverData, liveChannels);
    buildCoverTextures(liveCovers, liveCoverData, liveChannels.size());
}

// ---- Genre/category filter (Shows/Movies tabs) ------------------------------

static std::vector<std::string> collectGenres(const std::vector<Entry>& entries) {
    std::vector<std::string> g = {"All genres"};
    for (const auto& e : entries)
        if (!e.genre.empty() && std::find(g.begin(), g.end(), e.genre) == g.end())
            g.push_back(e.genre);
    return g;
}
static std::vector<Entry> filterByGenre(const std::vector<Entry>& entries,
                                        const std::vector<std::string>& genres,
                                        int idx) {
    if (idx <= 0 || idx >= (int)genres.size()) return entries; // 0 = "All genres"
    std::vector<Entry> out;
    for (const auto& e : entries) if (e.genre == genres[idx]) out.push_back(e);
    return out;
}
// Recomputes *Filtered from the full entries list for the given tab and
// resets its selection, matching Pluto3DS's changeGenre()/filter() reset.
static void rebuildGenreFilter(HomeMenu m) {
    if (m == HOME_SHOWS) {
        showsFiltered = filterByGenre(showsEntries, showsGenres, selShowGenre);
        selShows = 0;
    } else if (m == HOME_MOVIES) {
        moviesFiltered = filterByGenre(moviesEntries, moviesGenres, selMovieGenre);
        selMovies = 0;
    }
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

// ---- Loading helper ----------------------------------------------------------

// Queues the blocking load a given home tab needs (matching Pluto3DS's
// per-tab lazy loading: the guide loads on its own, Shows/Movies share one
// vod() fetch). Used both for L/R tab switches and for retrying after an
// error.
static void requestLoad(HomeMenu m) {
    if (m == HOME_LIVETV) {
        loadMsg = "Loading guide...";
        pending = LOAD_LIVETV;
    } else {
        loadMsg = "Loading movies and shows...";
        pending = LOAD_VOD;
    }
    state = STATE_LOADING;
}

// ---- Playback ---------------------------------------------------------------
// No UI state of its own: resolve the entry, tear the GPU context down,
// block in playerPlay() (with a seek-retry loop and, for live channels, the
// same renewal wiring Pluto3DS uses), then bring the GPU context back up and
// resume wherever the UI loop left off. Mirrors Pluto3DS's main.cpp control
// flow rather than 3DSfinPlus's STATE_PLAYER.

static void playEntry(const Entry& e) {
    Playback data = catalog.resolve(e);
    if (!data.error.empty() || data.url.empty()) {
        errorMsg = !data.error.empty() ? data.error
                 : "Could not get a playable stream for \"" + e.title + "\".";
        state = STATE_ERROR;
        return;
    }

    // Free GPU textures before tearing down the 3D context (playback owns
    // both screens and all of VRAM while it runs). Only the Live TV guide
    // has any to free.
    freeCovers(liveCovers);
    delete ui; ui = nullptr;
    graphicsStop();

    bool live = (e.kind == "channel");
    if (live) {
        // No in-player guide overlay in this first cut; only the renewal
        // callback is wired, so a channel whose playlist expires mid-session
        // gets a fresh one instead of just dying.
        // attempt starts at 1: attempt 0 (the lowest-bandwidth rendition)
        // was already tried by the catalog.resolve(e) call above, so the
        // first renewal should step to the next rung rather than repeat it
        // (see the variantAttempt comment on Catalog::resolve).
        playerRenewUrl = [e, attempt = 1]() mutable -> std::pair<std::string, std::string> {
            Playback fresh = catalog.resolve(e, attempt++);
            return { fresh.url, fresh.audioUrl };
        };
    }

    double offset = 0, seek = -1;
    bool ok = true;
    do {
        seek = -1;
        ok = playerPlay(data.url, (long long)(data.duration * 10000000.0),
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
    if (liveLoaded) buildCoverTextures(liveCovers, liveCoverData, liveChannels.size());

    // playerPlay() returns false on MVD init/allocation failure (Old 3DS, or
    // out of memory) rather than actually playing anything; surface that
    // instead of silently landing back on the grid with no explanation.
    if (!ok) {
        errorMsg = "Playback failed to start for \"" + e.title +
                   "\".\n\nCheck sdmc:/3ds/pluto3ds/player_debug.txt for details.";
        state = STATE_ERROR;
    }
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
    // player.cpp (copied unchanged from Pluto3DS) hardcodes its debug log
    // path to sdmc:/3ds/pluto3ds/player_debug.txt -- fopen() there silently
    // fails (and playback just runs without a log) unless that directory
    // already exists, which it only would if Pluto3DS was also ever
    // installed on this SD card. Create it here so the log is always
    // available for diagnosing a failed/frozen playback attempt.
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/pluto3ds", 0777);
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
                case LOAD_VOD: {
                    std::vector<Entry> all = catalog.vod();
                    if (!catalog.error.empty() && all.empty()) {
                        errorMsg = catalog.error;
                        state = STATE_ERROR;
                        break;
                    }
                    showsEntries.clear();
                    moviesEntries.clear();
                    for (auto& e : all) {
                        if (e.kind == "series")      showsEntries.push_back(std::move(e));
                        else if (e.kind == "movie")  moviesEntries.push_back(std::move(e));
                    }
                    showsGenres  = collectGenres(showsEntries);
                    moviesGenres = collectGenres(moviesEntries);
                    selShowGenre = 0;
                    selMovieGenre = 0;
                    showsFiltered  = showsEntries;
                    moviesFiltered = moviesEntries;
                    selShows = 0;
                    selMovies = 0;
                    vodLoaded = true;
                    state = STATE_HOME;
                    break;
                }

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

                case LOAD_SEARCH: {
                    if (!browseStack.empty()) browseStack.clear();
                    ItemLevel lv;
                    lv.title = "Search: " + searchQuery;
                    lv.items = catalog.search(searchQuery);
                    browseStack.push_back(std::move(lv));
                    state = STATE_ITEMS;
                    break;
                }

                case LOAD_EPISODES: {
                    ItemLevel lv;
                    lv.title = drillSeries.title;
                    lv.items = catalog.episodes(drillSeries);
                    browseStack.push_back(std::move(lv));
                    state = STATE_ITEMS;
                    break;
                }

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
                // L/R cycle the three home menus.
                if (kDown & (KEY_L | KEY_R)) {
                    int dir = (kDown & KEY_R) ? 1 : 2; // +1 or -1 (mod 3)
                    homeMenu = (HomeMenu)(((int)homeMenu + dir) % 3);
                    if (homeMenu == HOME_LIVETV && !liveLoaded) requestLoad(homeMenu);
                    else if (homeMenu != HOME_LIVETV && !vodLoaded) requestLoad(homeMenu);
                    break;
                }

                // Touch: edge-detected tap (KEY_TOUCH is a held state, so a
                // prev-frame flag turns it into a single "just tapped" event).
                bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
                int  touchHit = -1;
                bool touchSearch = false;
                // Grid nav/selection/touch all operate on the genre-filtered
                // list (see rebuildGenreFilter()); the unfiltered
                // showsEntries/moviesEntries only feed the genre list itself
                // and get re-filtered when it changes.
                std::vector<Entry>* curList = (homeMenu == HOME_SHOWS) ? &showsFiltered
                                             : (homeMenu == HOME_MOVIES) ? &moviesFiltered
                                             : nullptr;
                int* curSel = (homeMenu == HOME_SHOWS) ? &selShows
                            : (homeMenu == HOME_MOVIES) ? &selMovies
                            : nullptr;
                std::vector<std::string>* curGenres = (homeMenu == HOME_SHOWS) ? &showsGenres
                                                     : (homeMenu == HOME_MOVIES) ? &moviesGenres
                                                     : nullptr;
                int* curGenreIdx = (homeMenu == HOME_SHOWS) ? &selShowGenre
                                 : (homeMenu == HOME_MOVIES) ? &selMovieGenre
                                 : nullptr;
                if (touching && !homeTouchWasHeld) {
                    touchPosition touch; hidTouchRead(&touch);
                    if (curList && touch.px >= UI::BOT_W - 72 && touch.py < 24) {
                        touchSearch = true;
                    } else if (curList) {
                        touchHit = UI::hitTestBottomGrid(touch.px, touch.py,
                                                          (int)curList->size(), *curSel);
                    } else {
                        touchHit = UI::hitTestLiveList(touch.px, touch.py,
                                                        (int)liveChannels.size(), selLive);
                    }
                }
                homeTouchWasHeld = touching;

                if (curList) {
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

                    if (kDown & (KEY_X | KEY_SELECT)) {
                        int gd = (kDown & KEY_X) ? 1 : -1;
                        int gn = (int)curGenres->size();
                        *curGenreIdx = (*curGenreIdx + gd + gn) % gn;
                        rebuildGenreFilter(homeMenu);
                        break;
                    }

                    int n    = (int)curList->size();
                    int cols = UI::BGRID_COLS;
                    if (touchHit >= 0) *curSel = touchHit;
                    if (kDown & KEY_RIGHT && *curSel < n - 1 && (*curSel % cols) != cols - 1) (*curSel)++;
                    if (kDown & KEY_LEFT  && (*curSel % cols) != 0)                            (*curSel)--;
                    if (kDown & KEY_DOWN  && *curSel + cols < n)                               *curSel += cols;
                    if (kDown & KEY_UP    && *curSel - cols >= 0)                               *curSel -= cols;

                    if (kDown & KEY_A && n > 0) {
                        const Entry& e = (*curList)[*curSel];
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
                    browseStack.pop_back();
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
                    bool haveAnything = vodLoaded || liveLoaded;
                    if (haveAnything) state = STATE_HOME;
                    else               requestLoad(homeMenu);
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
                if (homeMenu == HOME_LIVETV) {
                    ui->drawLiveTvGuide(liveChannels, liveCovers, selLive);
                } else if (homeMenu == HOME_SHOWS) {
                    std::string title = "Shows";
                    if (selShowGenre > 0) title += " - " + showsGenres[selShowGenre];
                    ui->drawContentGrid(showsFiltered, {}, selShows, UI::TAB_SHOWS, title);
                } else {
                    std::string title = "Movies";
                    if (selMovieGenre > 0) title += " - " + moviesGenres[selMovieGenre];
                    ui->drawContentGrid(moviesFiltered, {}, selMovies, UI::TAB_MOVIES, title);
                }
                break;

            case STATE_ITEMS: {
                ItemLevel& lv = browseStack.back();
                ui->drawItemGrid(lv.items, {}, lv.sel, lv.title);
                break;
            }

            case STATE_ERROR:
                ui->drawErrorScreen(errorMsg);
                break;
        }

        ui->endFrame();
    }

    freeCovers(liveCovers);
    browseStack.clear();
    delete ui;
    graphicsStop();
    if (R_SUCCEEDED(httpRes)) httpcExit();
    gfxExit();
    return 0;
}
