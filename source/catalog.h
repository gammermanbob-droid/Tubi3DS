#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include "picojson.h"

// id/title/genre/kind/url/logo/now/next/duration — same shape Pluto3DS uses,
// reused here so player.cpp (copied over unchanged) needs no adjustment.
// kind is one of: "channel", "movie", "series", "episode".
// url: for a channel, the EPG's video_resources[0].manifest.url. For a movie
// or episode returned by vod()/search()/episodes(), usually empty — resolve()
// fetches the title's own page to get its video_resources, same as Tubi's
// website does, since vod()/search() results don't carry a manifest URL.
struct Entry {
    std::string id,title,genre,kind,url,logo,now,next;
    double duration=0;
};

struct Playback {
    std::string url,audioUrl,subtitleVtt,error;
    std::string subtitleDebug;
    double duration=0;
};

// One entry in Tubi's category browser. name is what's shown in the picker;
// slug is the URL path segment; pathPrefix is which section of the site it
// lives under -- confirmed these differ per group (Genres/Popular/
// Collections: "category"; Networks: "networks"; Hubs: "hubs", NOT
// "category" despite otherwise looking like one) by fetching
// https://tubitv.com/categories and reading each link's actual href rather
// than assuming a single pattern.
struct Category {
    std::string name, slug, pathPrefix;
};

// A titled group of categories, purely for how the picker displays them
// (Hubs/Popular/Genres/Collections/Networks) -- Tubi itself has no API for
// this list, so both the grouping and the slugs below are hand-collected
// from https://tubitv.com/categories (see categoryGroups() in catalog.cpp).
struct CategoryGroup {
    std::string title;
    std::vector<Category> items;
};

// The full, hand-collected category list shown by the Browse Categories
// screen, grouped for display. Static data (not fetched -- Tubi's category
// list itself isn't exposed anywhere machine-readable); see catalog.cpp for
// the source and how to refresh it if Tubi adds/renames categories later.
const std::vector<CategoryGroup>& categoryGroups();

// Tubi needs no session/login for any of this — every endpoint below is the
// same unauthenticated request Tubi's own website makes. That also means,
// unlike Pluto3DS's Catalog, there is no boot()/token/expires bookkeeping.
class Catalog {
    picojson::value json(const std::string& url);
    // Fetches an ordinary Tubi web page and pulls out its embedded
    // `window.__data = {...}` (or `window.__REACT_QUERY_STATE__`) blob,
    // repaired into parseable JSON. Every VOD-browsing entry point (vod(),
    // search(), episodes()) goes through this, since Tubi doesn't expose a
    // plain REST catalog API the way it does for live-channel EPG data
    // (channels() below uses a real JSON endpoint instead and doesn't need
    // this at all).
    picojson::value scrapePageData(const std::string& url, const char* varName = "window.__data");
public:
    std::string error;
    Catalog() {}

    // Live linear channels ("Tubi TV"), each with its current program (see
    // Entry::now) and a ready-to-play manifest URL (Entry::url) — both come
    // straight from the EPG response, so resolve() for a channel just parses
    // that URL directly and never has to fetch anything else first.
    std::vector<Entry> channels();

    // On-demand home shelves (movies/series). Best-effort: Tubi has no
    // documented public catalog API, so this scrapes the same embedded JSON
    // the website itself hydrates from. If Tubi's page structure doesn't
    // match what this looks for, it degrades to an empty list rather than
    // crashing — see the implementation comment for how to re-diagnose it
    // from a saved page (same approach used to build this in the first
    // place).
    std::vector<Entry> vod();

    // Search results (movies/series matching a free-text title). Same
    // best-effort scraping approach as vod().
    std::vector<Entry> search(const std::string& query);

    // A series' episodes, flattened across all seasons in order.
    // Best-effort, same caveat as vod().
    std::vector<Entry> episodes(const Entry& series);

    // A single category/network/hub's titles (e.g. Anime, Sci-fi & Fantasy,
    // A24) -- lets Browse Categories reach titles vod()'s homepage shelves
    // and search() (which only searches what vod() already loaded) never
    // surface. Same scrapePageData()+collectShelves() approach as vod(),
    // just pointed at tubitv.com/<cat.pathPrefix>/<cat.slug> — confirmed
    // that page's containerChildrenIdMap is populated the same way even
    // though its containersList (home-page shelf ordering only) is empty
    // there. Same best-effort/empty-list-on-miss caveat as vod().
    std::vector<Entry> category(const Category& cat);

    // Resolves a playable Entry to an actual HLS URL. For a channel this is
    // just hls::parse()+variant-select on Entry::url. For a movie/episode
    // (which normally arrives with url empty), it first fetches the title's
    // own page to read its video_resources — the same window.__data lookup
    // yt-dlp's Tubi extractor uses — before doing the same variant-select.
    //
    // variantAttempt selects which rung of the ABR ladder to use, sorted
    // ascending by bandwidth and taken modulo the variant count: 0 (the
    // default) is the lowest-bandwidth rendition, matching the original
    // behavior for the first resolve of any title. A live channel's renewal
    // path (see playerRenewUrl in main.cpp) increments this on each retry,
    // so repeated playback failures step through progressively higher
    // rungs instead of re-resolving the exact same URL forever — observed
    // live-TV symptom: a specific low-bitrate rendition can be dead on
    // Tubi's origin while the rest of that channel's ladder is healthy, and
    // without this the renewal loop just re-picks the same dead URL every
    // single cycle (see player_debug.txt's repeating "playlist HTTP=404").
    Playback resolve(const Entry& entry, int variantAttempt = 0);
};
