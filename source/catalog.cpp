#include "catalog.h"
#include "http.h"
#include "hls.h"
#include <algorithm>
#include <set>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <ctime>

// ---- Live-channel diagnostics ----------------------------------------------
// Three prior fixes (skip the fMP4 probe for live playback, cycle the ABR
// ladder on renewal instead of re-picking the same rung, and normalize/
// DRM-filter the channel manifest URL like the reference scraper does) each
// looked like a plausible cause of a live-channel HTTP 404 loop, and none of
// them changed the symptom -- confirmed identical across two entirely
// different channels (NASCAR, then UEFA TV), so this isn't one channel's
// broken rendition or one channel's DRM-ordered resource list. Rather than
// guess a fifth fix blindly, this appends ground truth to a dedicated
// catalog_debug.txt (kept separate from playerPlay's player_debug.txt, which
// is opened in truncating "w" mode well after Catalog::resolve() already
// ran): every channel's video_resources types as channels() sees them, and
// -- for whichever channel actually gets played -- the exact master
// playlist URL used and a redacted dump of what Tubi's origin actually sent
// back for it. That settles, directly, whether the master response itself
// is legitimate live content or something generic/wrong, instead of
// continuing to infer it indirectly from which derived rendition 404s.
static void appendDebug(const char* fmt, ...) {
    FILE* f = fopen("sdmc:/3ds/pluto3ds/catalog_debug.txt", "a");
    if (!f) return;
    va_list args; va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}
// Redacts every line's query string (same "?<redacted>" convention used
// throughout this codebase) so a raw playlist-body dump can be pasted back
// without leaking signed tokens.
static std::string redactLines(const std::string& text, size_t maxBytes) {
    std::string src = text.size() > maxBytes ? text.substr(0, maxBytes) : text;
    std::string out;
    size_t start = 0;
    while (start <= src.size()) {
        size_t nl = src.find('\n', start);
        std::string line = src.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t q = line.find('?');
        out += (q == std::string::npos ? line : line.substr(0, q) + "?<redacted>");
        out += '\n';
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (text.size() > maxBytes) out += "...(truncated)\n";
    return out;
}
static std::string redactUrl(const std::string& url) {
    size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q) + "?<redacted>";
}

// ---- picojson helpers (same defensive pattern Pluto3DS's catalog.cpp uses:
// every accessor tolerates a missing/wrong-typed key instead of throwing) ---
static const picojson::value& field(const picojson::value& v,const char* key) {
    static picojson::value empty;
    return v.is<picojson::object>()?v.get(key):empty;
}
static std::string str(const picojson::value& v,const char* key) {
    const auto& x=field(v,key);
    if (x.is<std::string>()) return x.get<std::string>();
    if (x.is<double>()) { char b[32]; snprintf(b,sizeof(b),"%.0f",x.get<double>()); return b; }
    return "";
}
static double number(const picojson::value& v,const char* key) {
    const auto& x=field(v,key);return x.is<double>()?x.get<double>():0;
}
static const picojson::array& array(const picojson::value& v) {
    static picojson::array empty;return v.is<picojson::array>()?v.get<picojson::array>():empty;
}
static const picojson::array& arrayField(const picojson::value& v,const char* key) {
    return array(field(v,key));
}

// ---- Live-channel manifest URL normalization -------------------------------
// channels()' EPG rows carry each rendition's manifest URL as a raw JSON
// string in video_resources[*].manifest.url, taken as-is by str() above --
// no percent-decoding, since JSON string escaping and URL percent-encoding
// are different layers and picojson only undoes the former. The reference
// implementation this scraper (channels()) and its EPG shape were built
// against, BuddyChewChew/tubi-scraper (see the channels() comment above),
// treats that same field very differently before using it: it runs it
// through Python's urllib.parse.unquote() (percent-decode only -- unlike
// unquote_plus, '+' is left alone) and then strips any query string and
// fragment entirely (scheme+netloc+path only). This code originally used
// the raw, still-encoded, query-intact URL unchanged, which is suspected of
// causing every rendition Catalog::resolve() derives from it to 404
// identically and permanently for at least one live channel (NASCAR) --
// see player_debug.txt logs cycling cleanly through 426x240/640x360/
// 848x480/1280x720 and getting HTTP=404 on literally every one, which reads
// far more like "the master itself is a generic/placeholder response for a
// malformed request" than "one specific rendition is dead upstream".
static std::string urlDecode(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i=0; i<s.size(); ++i) {
        if (s[i]=='%' && i+2<s.size() &&
            isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2])) {
            auto hexVal=[](char c)->int{ return c<='9' ? c-'0' : (tolower(c)-'a'+10); };
            out += static_cast<char>((hexVal(s[i+1])<<4) | hexVal(s[i+2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}
static std::string stripQueryFragment(const std::string& url) {
    auto pos = url.find_first_of("?#");
    return pos==std::string::npos ? url : url.substr(0,pos);
}

// Forward declaration: defined further down (see its own comment there), but
// channels() below now needs it too -- see the note where it's used.
static std::string pickManifestUrl(const picojson::value& videoObj);
static const picojson::object& objectOf(const picojson::value& v) {
    static picojson::object empty;return v.is<picojson::object>()?v.get<picojson::object>():empty;
}
// Tubi's poster/logo URLs are protocol-relative ("//canvas-lb.tubitv.com/...").
static std::string fixUrl(std::string u) {
    if (u.rfind("//",0)==0) return "https:"+u;
    return u;
}
static std::string firstImage(const picojson::value& v,const char* key) {
    const auto& a=arrayField(v,key);
    for (const auto& im:a) if (im.is<std::string>()) return fixUrl(im.get<std::string>());
    return "";
}

// ---- window.__data / window.__REACT_QUERY_STATE__ scraping ----------------
// Tubi's website has no plain REST catalog API (confirmed: /search/<query>
// renders nothing server-side — results are fetched by client-side JS this
// scraper can't observe). Every page instead hydrates React from one big
// inline `window.<name> = {...};` object, so this pulls that object's JSON
// out of the raw HTML with a string/escape-aware balanced-brace scan (same
// technique used elsewhere in this codebase to verify brace balance), then
// repairs the two things that make it invalid JSON: bare `undefined` and
// `new Date("...")` calls, both of which the site happily emits since it's
// JS, not JSON.
static bool extractBalancedObject(const std::string& html, size_t braceStart, std::string& out) {
    int depth=0; bool inStr=false; char strCh=0; bool esc=false;
    size_t i=braceStart;
    for (; i<html.size(); ++i) {
        char c=html[i];
        if (inStr) {
            if (esc) esc=false;
            else if (c=='\\') esc=true;
            else if (c==strCh) inStr=false;
            continue;
        }
        if (c=='"' || c=='\'') { inStr=true; strCh=c; continue; }
        if (c=='{') ++depth;
        else if (c=='}') { --depth; if (depth==0) { ++i; break; } }
    }
    if (depth!=0) return false;
    out = html.substr(braceStart, i-braceStart);
    return true;
}
static void jsonRepair(std::string& s) {
    // `undefined` -> `null`, word-boundary only (never touch e.g. "isUndefined").
    size_t p=0;
    while ((p=s.find("undefined",p))!=std::string::npos) {
        bool left = p==0 || !(isalnum((unsigned char)s[p-1]) || s[p-1]=='_');
        size_t end=p+9;
        bool right = end>=s.size() || !(isalnum((unsigned char)s[end]) || s[end]=='_');
        if (left && right) { s.replace(p,9,"null"); p+=4; } else p=end;
    }
    // `new Date("...")` -> `"..."`
    p=0;
    while ((p=s.find("new Date(",p))!=std::string::npos) {
        size_t argStart=p+9;
        if (argStart<s.size() && s[argStart]=='"') {
            size_t q=s.find('"',argStart+1);
            if (q!=std::string::npos) {
                size_t close=s.find(')',q+1);
                if (close!=std::string::npos) {
                    std::string lit=s.substr(argStart,q-argStart+1);
                    s.replace(p,close-p+1,lit);
                    p+=lit.size();
                    continue;
                }
            }
        }
        p+=9;
    }
}
picojson::value Catalog::scrapePageData(const std::string& url, const char* varName) {
    picojson::value out;
    auto r=get(url);
    if (!r.ok()) { error="Could not load Tubi page ("+std::to_string(r.status)+")"; return out; }
    auto pos=r.body.find(varName);
    if (pos==std::string::npos) { error="Tubi page had no "+std::string(varName); return out; }
    auto eq=r.body.find('=',pos);
    auto brace=r.body.find('{',eq==std::string::npos?pos:eq);
    if (brace==std::string::npos) { error="Tubi page had no "+std::string(varName)+" object"; return out; }
    std::string blob;
    if (!extractBalancedObject(r.body,brace,blob)) { error="Could not read "+std::string(varName); return out; }
    jsonRepair(blob);
    auto perr=picojson::parse(out,blob);
    if (!perr.empty()) error="Tubi page's "+std::string(varName)+" was unreadable";
    return out;
}

// ---- Live channels (JSON API, no scraping needed) --------------------------
static std::string iso(time_t t) { char b[40];strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",gmtime(&t));return b; }

picojson::value Catalog::json(const std::string& url) {
    picojson::value data;
    auto r=get(url);
    if (!r.ok()) { error="Could not reach Tubi ("+std::to_string(r.status)+")"; return data; }
    auto perr=picojson::parse(data,r.body);
    if (!perr.empty()) error="Tubi returned an unreadable response";
    return data;
}

std::vector<Entry> Catalog::channels() {
    error.clear();
    // Step 1: /live's window.__data only carries the ordered list of live
    // channel content ids (data.epg.contentIdsByContainer[*][*].contents) —
    // everything else (name, logo, programs, playable URL) comes from the
    // EPG endpoint in step 2. Confirmed against a live BuddyChewChew/tubi-
    // scraper-style implementation; see catalog.h for the general approach.
    auto page = scrapePageData("https://tubitv.com/live");
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const auto& containerEntry : objectOf(field(field(page,"epg"),"contentIdsByContainer"))) {
        for (const auto& category : array(containerEntry.second)) {
            for (const auto& cid : arrayField(category,"contents")) {
                std::string id = cid.is<std::string>() ? cid.get<std::string>() : str(category,"__never__");
                if (id.empty() && cid.is<double>()) { char b[32]; snprintf(b,sizeof(b),"%.0f",cid.get<double>()); id=b; }
                if (!id.empty() && seen.insert(id).second) ids.push_back(id);
            }
        }
    }
    if (ids.empty()) { if (error.empty()) error="No live channels found"; return {}; }

    // Step 2: batch the ids into the EPG endpoint (Tubi's own site batches
    // in groups of 150; matched here for the same reason — a URL with every
    // channel id at once risks hitting a server-side length limit).
    std::vector<Entry> out;
    time_t now = time(nullptr);
    std::string nowIso = iso(now);
    for (size_t i=0; i<ids.size(); i+=150) {
        std::string csv;
        for (size_t j=i; j<ids.size() && j<i+150; ++j) { if (!csv.empty()) csv+=','; csv+=ids[j]; }
        auto data = json("https://tubitv.com/oz/epg/programming?content_id="+csv);
        if (!error.empty()) return out;   // partial results aren't worth the confusion of a mixed list
        for (const auto& row : arrayField(data,"rows")) {
            Entry e;
            e.id    = str(row,"content_id");
            e.title = str(row,"title");
            e.kind  = "channel";
            e.logo  = firstImage(field(row,"images"),"thumbnail");
            // Was: take the FIRST non-empty video_resources[*].manifest.url,
            // with no regard for its "type". video_resources can (and here,
            // demonstrably does for at least one channel -- see the NASCAR
            // 404 investigation in resolve()'s comment) list DRM-flagged or
            // DASH entries ahead of the actual clear HLS one; VOD's own
            // lookup (pickManifestUrl, below) already knows to skip those
            // and prefer hlsv3/hlsv6 -- reusing it here instead of
            // duplicating a naive version fixes channels() the same way.
            e.url = pickManifestUrl(row);
            // Diagnostic (see the block comment near appendDebug/redactUrl
            // above): dump every channel's video_resources types and which
            // one got picked, once per channels() call (guide load), so we
            // can see directly whether DRM/DASH entries are actually
            // present/ordered the way the 404-loop investigation suspected
            // -- across the whole channel list, not just whichever one gets
            // played.
            {
                std::string types;
                for (const auto& res : arrayField(row,"video_resources")) {
                    if (!types.empty()) types += ",";
                    std::string t = str(res,"type");
                    types += t.empty() ? "(none)" : t;
                }
                appendDebug("channel id=%s title=%s resources=[%s] picked=%s\n",
                            e.id.c_str(), e.title.c_str(), types.c_str(),
                            redactUrl(e.url).c_str());
            }
            for (const auto& prog : arrayField(row,"programs")) {
                auto start=str(prog,"start_time"), stop=str(prog,"end_time");
                if (start<=nowIso && stop>nowIso) e.now=str(prog,"title");
                else if (start>nowIso && e.next.empty()) e.next=str(prog,"title");
            }
            if (!e.id.empty() && !e.title.empty()) out.push_back(e);
        }
    }
    return out;
}

// ---- On-demand (movies/series) ---------------------------------------------
// Best-effort: built from the same window.__data.container / window.__data.video
// structure the Tubi homepage itself hydrates from (confirmed by saving and
// inspecting a real page — see the project's tubi_debug/ notes). Tubi has no
// plain REST catalog endpoint, so this is the only way to browse without
// reimplementing a JS client. If Tubi changes this structure, the symptom is
// an empty (not crashing) home shelf list — re-diagnose the same way this was
// built: save `curl https://tubitv.com/` to a file and look at what
// window.__data.container/.video actually contain now.
static std::vector<Entry> collectShelves(const picojson::value& page, std::string* errOut) {
    std::vector<Entry> out;
    std::set<std::string> seenIds;
    const auto& idMap         = objectOf(field(field(page,"container"),"containerIdMap"));
    const auto& childrenMap   = objectOf(field(field(page,"container"),"containerChildrenIdMap"));
    const auto& videoById     = objectOf(field(field(page,"video"),"byId"));

    // Walk containerChildrenIdMap's own keys rather than containersList:
    // containersList only orders the home page's shelves — a /category/<slug>
    // page (confirmed by inspecting a saved response) leaves it empty even
    // though containerChildrenIdMap itself is populated for that category.
    for (const auto& shelfEntry : childrenMap) {
        const std::string& shelfId = shelfEntry.first;
        auto shelfMetaIt = idMap.find(shelfId);
        std::string shelfTitle = shelfMetaIt!=idMap.end() ? str(shelfMetaIt->second,"title") : shelfId;
        for (const auto& contentIdVal : array(shelfEntry.second)) {
            if (!contentIdVal.is<std::string>()) continue;
            std::string contentId = contentIdVal.get<std::string>();
            if (!seenIds.insert(contentId).second) continue;   // same title can appear on several shelves
            auto videoIt = videoById.find(contentId);
            if (videoIt==videoById.end()) continue;
            const auto& v = videoIt->second;
            std::string detailedType = str(v,"detailed_type");
            Entry e;
            e.id    = contentId;
            e.title = str(v,"title");
            e.genre = shelfTitle;
            e.logo  = firstImage(v,"posterarts");
            e.duration = number(v,"duration");
            if (detailedType=="movie") e.kind="movie";
            else if (detailedType=="series") e.kind="series";
            else continue; // skip anything not clearly a movie or series
            if (!e.id.empty() && !e.title.empty()) out.push_back(e);
        }
    }
    if (out.empty() && errOut) *errOut = "No on-demand titles found (Tubi's page layout may have changed)";
    return out;
}

std::vector<Entry> Catalog::vod() {
    error.clear();
    auto page = scrapePageData("https://tubitv.com/");
    if (!error.empty()) return {};
    return collectShelves(page, &error);
}

// ---- Category browser --------------------------------------------------
// Hand-collected from https://tubitv.com/categories (server-rendered static
// HTML — unlike vod()/search(), this page does NOT hydrate from window.__data,
// so there's nothing to scrape here; the list itself has to be maintained by
// hand). To refresh: fetch that URL and read each section's actual link
// hrefs (do NOT assume a single path pattern -- confirmed by inspecting the
// real page that Hubs links are /hubs/<slug>, Networks are /networks/<slug>,
// and everything else is /category/<slug>; several slugs also don't match
// their display name at all, e.g. Classic TV & Movies -> "classics",
// Indie Movies -> "indie_films", The YA Edit -> "ya_forever"). The grouping
// below (Hubs/Popular/Genres/Collections/Networks) mirrors the page's own
// section headings as of this writing.
const std::vector<CategoryGroup>& categoryGroups() {
    static const std::vector<CategoryGroup> groups = {
        { "Hubs", {
            {"Creatorverse","creatorverse-hub","hubs"},
            {"Terror on Tubi","terror-on-tubi-hub","hubs"},
        }},
        { "Popular", {
            {"Black Storytelling","black_cinema","category"},
            {"Most Popular","most_popular","category"},
            {"Recently Added","recently_added","category"},
            {"Recommended","recommended_for_you","category"},
            {"Tubi Originals","tubi_originals","category"},
        }},
        { "Genres", {
            {"Action","action","category"},
            {"Adult Animation","adult_animation","category"},
            {"Anime","anime","category"},
            {"Classic TV & Movies","classics","category"},
            {"Comedy","comedy","category"},
            {"Crime TV","crime_tv","category"},
            {"Documentaries","documentary","category"},
            {"Docuseries","docuseries","category"},
            {"Drama","drama","category"},
            {"Family Movies","family_movies","category"},
            {"Family Shows","family_series","category"},
            {"Horror","horror","category"},
            {"Indie Movies","indie_films","category"},
            {"LGBTQ+ Storytelling","lgbt","category"},
            {"Lifestyle","lifestyle_tv","category"},
            {"Music","music","category"},
            {"Mystery","mystery","category"},
            {"Podcasts","podcast","category"},
            {"Reality TV","reality_tv","category"},
            {"Romance","romance","category"},
            {"Sci-fi & Fantasy","sci_fi_and_fantasy","category"},
            {"Science & Nature","science_and_nature_acc","category"},
            {"Sports Stories","sports_movies_and_tv","category"},
            {"Thrillers","thrillers","category"},
            {"True Crime","true_crime","category"},
            {"Westerns","westerns","category"},
        }},
        { "Collections", {
            {"Audio Description","audio_description","category"},
            {"Back To School","back_to_school","category"},
            {"Double Features","double_features","category"},
            {"Family Franchise Faves","family_franchises","category"},
            {"Franchise Fever","franchise_fever","category"},
            {"K-Horror Nightmares","korean_horror","category"},
            {"Martin Scorsese Essentials","martin_scorsese","category"},
            {"Spy Movies","spy_movies","category"},
            {"Superheroes","superheroes","category"},
            {"The YA Edit","ya_forever","category"},
            {"Transformers Universe","transformers_universe","category"},
            {"Trophy-Worthy TV","trophy_worthy_tv","category"},
            {"Witch, Please","witch_please","category"},
        }},
        { "Networks", {
            {"A&E","aetv","networks"},
            {"A24","a24","networks"},
            {"CJ ENM Selects","cj_enm","networks"},
            {"Classic Doctor Who","classic_doctor_who","networks"},
            {"CONtv","contv","networks"},
            {"Docurama","docurama","networks"},
            {"Dove Channel","dovechannel","networks"},
            {"DUST","dust","networks"},
            {"FilmRise","filmriseclassictv","networks"},
            {"FOX","fox","networks"},
            {"FOX SOUL","fox_soul","networks"},
            {"FOX Sports","fox_sports","networks"},
            {"Full Moon Features","full_moon_features","networks"},
            {"Hallmark","hallmark","networks"},
            {"Lifetime","lifetime","networks"},
            {"LOL! Network","lol_network","networks"},
            {"Sesame Street","sesame_workshop","networks"},
            {"Shout! Factory TV","shoutfactorytv","networks"},
            {"So...Real","so_real","networks"},
            {"TMZ Presents","tmz","networks"},
            {"TV One","tvone","networks"},
            {"UNINTERRUPTED","uninterrupted","networks"},
        }},
    };
    return groups;
}

std::vector<Entry> Catalog::category(const Category& cat) {
    error.clear();
    auto page = scrapePageData("https://tubitv.com/"+cat.pathPrefix+"/"+cat.slug);
    if (!error.empty()) return {};
    return collectShelves(page, &error);
}

// No discoverable server-side search endpoint (see the comment above
// scrapePageData): this filters titles already visible across the home
// page's shelves rather than searching Tubi's full catalog. It will miss
// anything not currently featured on the homepage — a real limitation, not
// a bug, until Tubi's client-side search call gets found some other way.
std::vector<Entry> Catalog::search(const std::string& query) {
    error.clear();
    auto page = scrapePageData("https://tubitv.com/");
    if (!error.empty()) return {};
    auto all = collectShelves(page, nullptr);
    std::string needle; needle.reserve(query.size());
    for (char c : query) needle += (char)tolower((unsigned char)c);
    std::vector<Entry> out;
    std::set<std::string> seenIds;
    for (auto& e : all) {
        if (e.kind!="series" && e.kind!="movie") continue;
        std::string hay; hay.reserve(e.title.size());
        for (char c : e.title) hay += (char)tolower((unsigned char)c);
        if (hay.find(needle)!=std::string::npos && seenIds.insert(e.id).second) out.push_back(e);
    }
    if (out.empty()) error="No loaded titles matched \""+query+"\" (search only looks through titles Tubi's home page has already shown)";
    return out;
}

// A series' episodes. Confirmed against a real saved series page: every
// episode is a "detailed_type":"episode" entry in window.__data.video.byId,
// and its title already arrives pre-formatted as "S01:E01 - Episode Name" —
// no separate season/episode-number field needs to be read or reassembled.
std::vector<Entry> Catalog::episodes(const Entry& series) {
    error.clear();
    auto page = scrapePageData("https://tubitv.com/series/"+series.id+"/show");
    if (!error.empty()) return {};
    const auto& videoById = objectOf(field(field(page,"video"),"byId"));
    std::vector<Entry> out;
    for (const auto& kv : videoById) {
        const auto& v = kv.second;
        if (str(v,"detailed_type")!="episode") continue;
        Entry e;
        e.id    = kv.first;
        e.title = str(v,"title");
        e.kind  = "episode";
        e.logo  = firstImage(v,"posterarts");
        e.duration = number(v,"duration");
        if (!e.id.empty() && !e.title.empty()) out.push_back(e);
    }
    // Tubi's own S01:E01-prefixed titles sort correctly as plain strings.
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b){ return a.title<b.title; });
    if (out.empty()) error="No episodes found for this series";
    return out;
}

// ---- Playback ----------------------------------------------------------
// Shared by both VOD (resolve()'s movie/episode branch) and live channels
// (channels(), above) -- a video_resources array can list DRM-flagged or
// DASH entries alongside (or instead of) the actual playable clear HLS one,
// for either kind of content, so both need the same type-aware pick rather
// than blindly taking video_resources[0].
static std::string pickManifestUrl(const picojson::value& videoObj) {
    // Prefer classic segmented HLS (hlsv3/hlsv6) over anything DRM-flagged
    // (Widevine/FairPlay/PlayReady — logged by yt-dlp's Tubi extractor as
    // unplayable) or DASH (this codebase's hls.h only parses M3U8).
    const char* preference[] = {"hlsv3","hlsv6"};
    for (const char* want : preference) {
        for (const auto& res : arrayField(videoObj,"video_resources")) {
            if (str(res,"type")==want) {
                std::string u=str(field(res,"manifest"),"url");
                if (!u.empty()) return u;
            }
        }
    }
    // Fall back to the first resource that isn't obviously DRM/dash.
    for (const auto& res : arrayField(videoObj,"video_resources")) {
        std::string t=str(res,"type");
        if (t.find("widevine")!=std::string::npos || t.find("fairplay")!=std::string::npos ||
            t.find("playready")!=std::string::npos || t=="dash") continue;
        std::string u=str(field(res,"manifest"),"url");
        if (!u.empty()) return u;
    }
    return "";
}

// Tubi can offer an HEVC-coded rendition for a title (typically alongside,
// or instead of, an H.264 one for a 4K/HDR master) via the exact same
// video_resources/hlsv6 path pickManifestUrl() already prefers second --
// "hlsv6" itself says nothing about codec, only container/packaging, so
// this can't be filtered any earlier than here, where the actual master
// playlist's own #EXT-X-STREAM-INF CODECS attribute (already parsed into
// hls::Variant::codecs by hls::parse()) is the only thing that says what's
// really inside a given rendition. New3DS's MVD hardware decoder only
// understands H.264 (see player.cpp's mvdstdInit(..., MVD_INPUT_H264, ...))
// -- there is no HEVC decode path on this hardware, hardware or software
// (an ARM11 CPU has nowhere near the throughput for software HEVC decode at
// any watchable resolution), so an HEVC rendition has to be skipped in favor
// of an H.264 one whenever a master offers both, and the whole title
// reported as unplayable (not silently handed to MVD, which expects H.264
// NAL unit framing and would misparse HEVC's differently-shaped NAL headers
// rather than failing cleanly) when every rendition in it is HEVC.
static bool isHevcCodec(const std::string& codecs) {
    // CODECS is a comma-separated list per RFC 6381 (e.g. one video + one
    // audio entry); check each entry's own prefix rather than the whole
    // attribute string, and case-insensitively since the RFC's example
    // casing ("hev1", "hvc1") isn't binding on how any given origin emits it.
    size_t start = 0;
    while (start <= codecs.size()) {
        size_t comma = codecs.find(',', start);
        std::string one = codecs.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::string lower; lower.reserve(one.size());
        for (char c : one) lower += (char)tolower((unsigned char)c);
        // hev1/hvc1: plain HEVC. dvhe/dvh1: Dolby Vision profiles that are
        // HEVC-based (dual-layer or single-layer over HEVC) -- MVD can't
        // decode the HEVC base layer any more than a plain hev1/hvc1 one.
        if (lower.rfind("hev1",0)==0 || lower.rfind("hvc1",0)==0 ||
            lower.rfind("dvhe",0)==0 || lower.rfind("dvh1",0)==0) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

Playback Catalog::resolve(const Entry& e, int variantAttempt) {
    error.clear();
    Playback p;
    std::string manifestUrl = e.url;   // channels arrive with this already filled in
    if (!manifestUrl.empty()) {
        // Channel manifest URLs specifically (VOD's manifestUrl comes from
        // pickManifestUrl() below and is left untouched) get normalized the
        // same way the reference scraper channels()/pickManifestUrl's EPG
        // shape was built against (BuddyChewChew/tubi-scraper) treats this
        // exact field: percent-decoded, then stripped of any query string
        // and fragment. This code previously used the raw, still-encoded,
        // query-intact URL as-is -- suspected cause of a live channel where
        // every ABR rendition Catalog::resolve() derived from the fetched
        // master 404'd identically and permanently (see the variantAttempt
        // comment above): consistent with Tubi's origin returning a
        // generic/placeholder master for a malformed request rather than
        // the real one.
        manifestUrl = urlDecode(manifestUrl);
        manifestUrl = stripQueryFragment(manifestUrl);
    }
    if (manifestUrl.empty()) {
        // Movie or episode: fetch its own page (same lookup yt-dlp's Tubi
        // extractor uses) to get video_resources, which the shelf/search
        // listing deliberately omits to keep those payloads small.
        std::string path = e.kind=="episode" ? "tv-shows" : "movies";
        auto page = scrapePageData("https://tubitv.com/"+path+"/"+e.id+"/video");
        if (!error.empty()) { p.error=error; return p; }
        const auto& videoById = objectOf(field(field(page,"video"),"byId"));
        auto it = videoById.find(e.id);
        if (it==videoById.end()) { p.error="Tubi has no playback data for this title"; return p; }
        manifestUrl = pickManifestUrl(it->second);
    }
    if (manifestUrl.empty()) { p.error="No stream available for this title"; return p; }

    auto r = get(manifestUrl);
    // Diagnostic (see the block comment near appendDebug above): log exactly
    // what was fetched and what came back, redacted, for every resolve --
    // channel or VOD/episode alike (this used to be channel-only, gated on
    // `!e.url.empty()`, which meant a movie/episode's own master fetch was
    // never logged at all; there was no way to see what Tubi actually sent
    // back for one short of adding this by hand). This directly answers
    // whether Tubi's origin is serving genuine content for this URL or
    // something else, instead of continuing to infer it from which derived
    // rendition fails three layers downstream.
    appendDebug("\nresolve id=%s title=%s kind=%s attempt=%d masterUrl=%s finalUrl=%s httpStatus=%lu error=%08lX bodyBytes=%lu\n",
                e.id.c_str(), e.title.c_str(), e.kind.c_str(), variantAttempt, redactUrl(manifestUrl).c_str(),
                redactUrl(r.finalUrl).c_str(),
                (unsigned long)r.status, (unsigned long)r.error, (unsigned long)r.body.size());
    appendDebug("masterBody(first 1000B, redacted):\n%s\n", redactLines(r.body, 1000).c_str());
    if (!r.ok()) { p.error="Playback unavailable ("+std::to_string(r.status)+")"; return p; }
    auto master = hls::parse(r.body);
    // Log what hls::parse() actually found, unconditionally -- most
    // importantly master.subtitles, which nothing in this function has ever
    // looked at before now (Playback::subtitleVtt/subtitleDebug exist and
    // are threaded all the way through to player.cpp, but resolve() never
    // populated them). Logging this even in the empty case is deliberate: a
    // log with no subtitle-related lines was previously ambiguous between
    // "Tubi's master genuinely has no subtitle renditions" and "the code
    // just never checked" -- it was always the latter, since this parses
    // and inspects master.subtitles regardless.
    appendDebug("parsed: variants=%zu segments=%zu fragmented=%d audio=%zu subtitles=%zu\n",
                master.variants.size(), master.segments.size(), (int)master.fragmented,
                master.audio.size(), master.subtitles.size());
    if (master.subtitles.empty()) {
        appendDebug("  no subtitle renditions in master playlist\n");
    } else {
        for (const auto& s : master.subtitles) {
            appendDebug("  subtitle group=%s lang=%s default=%d forced=%d uri=%s\n",
                        s.groupId.c_str(), s.language.c_str(), (int)s.isDefault, (int)s.isForced,
                        redactUrl(s.uri).c_str());
        }
    }

    // get()/fetch() follows redirects internally and r.body is whatever the
    // LAST hop actually returned, but a variant URI inside that body which
    // happens to be an absolute path (leading '/' -- confirmed to be exactly
    // what Tubi's live master playlists use here, see catalog_debug.txt's
    // masterBody dumps) needs to be resolved against wherever the content
    // actually came from, not the URL that was originally requested. Using
    // manifestUrl (pre-redirect) here reconstructs every derived rendition
    // URL against the wrong host whenever the master fetch itself
    // redirects -- confirmed via http_debug.txt logging status=302 on every
    // single one of these live-channel master fetches, which had gone
    // completely unnoticed because get()/fetch() silently absorbs the
    // redirect chain and only ever handed callers the original request URL
    // to resolve against. This is suspected as the actual root cause behind
    // every prior fix (fMP4 probe skip, ABR cycling, URL normalize/DRM
    // filter, cookie jar) failing to change the symptom at all.
    const std::string& masterBaseUrl = r.finalUrl.empty() ? manifestUrl : r.finalUrl;

    std::string finalUrl;
    if (!master.variants.empty()) {
        // Drop any variant whose CODECS attribute explicitly names an HEVC
        // profile (see isHevcCodec() above) -- but only if that still leaves
        // at least one variant, since a master with no CODECS attribute at
        // all (some origins omit it) must not be treated as if every
        // variant were HEVC just because none could be positively confirmed
        // H.264 either. If every variant is confirmed HEVC, this whole
        // title has nothing this hardware can decode.
        std::vector<hls::Variant> nonHevc;
        for (const auto& v : master.variants) if (!isHevcCodec(v.codecs)) nonHevc.push_back(v);
        if (nonHevc.size() != master.variants.size()) {
            std::string allCodecs;
            for (const auto& v : master.variants) { allCodecs += v.codecs; allCodecs += ' '; }
            if (nonHevc.empty()) {
                appendDebug("ERROR: No H.264 variants found, codecs available: %s\n", allCodecs.c_str());
            } else {
                appendDebug("Filtered to %zu H.264 variant(s) from %zu total (codecs seen: %s)\n",
                            nonHevc.size(), master.variants.size(), allCodecs.c_str());
            }
        }
        if (nonHevc.empty()) {
            p.error = "This title is only available in HEVC, which the 3DS's video decoder can't play.";
            return p;
        }
        // Sorted ascending by bandwidth so attempt 0 keeps the original
        // lowest-bitrate choice; a nonzero variantAttempt (renewal retries
        // only -- see the header comment) steps to the next rung up instead
        // of landing back on the same one, and wraps rather than picking
        // something out of range.
        std::vector<hls::Variant> byBandwidth(nonHevc.begin(), nonHevc.end());
        std::sort(byBandwidth.begin(), byBandwidth.end(),
                  [](const hls::Variant& a,const hls::Variant& b){return a.bandwidth<b.bandwidth;});
        size_t idx = byBandwidth.size() > 1
                   ? (size_t)variantAttempt % byBandwidth.size()
                   : 0;
        const auto& v = byBandwidth[idx];
        finalUrl = hls::resolve(masterBaseUrl, v.uri);
        appendDebug("picked variant idx=%zu bandwidth=%u codecs=%s finalUrl=%s\n",
                    idx, v.bandwidth, v.codecs.c_str(), redactUrl(finalUrl).c_str());
        if (!v.audio.empty()) {
            std::string best;
            for (const auto& a : master.audio) {
                if (a.groupId!=v.audio) continue;
                if (best.empty() || a.isDefault) best=a.uri;
            }
            if (!best.empty()) p.audioUrl = hls::resolve(masterBaseUrl, best);
        }
    } else if (!master.segments.empty()) {
        // manifestUrl was already a media (not master) playlist.
        finalUrl = masterBaseUrl;
    }
    if (finalUrl.empty()) { p.error="No playable video formats"; return p; }
    p.url = finalUrl;
    p.duration = e.duration;
    return p;
}
