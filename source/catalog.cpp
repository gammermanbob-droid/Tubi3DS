#include "catalog.h"
#include "http.h"
#include "hls.h"
#include <algorithm>
#include <set>
#include <cctype>
#include <cstdio>
#include <ctime>

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
            for (const auto& res : arrayField(row,"video_resources")) {
                std::string u = str(field(res,"manifest"),"url");
                if (!u.empty()) { e.url=u; break; }
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
// Picks the lowest-bitrate non-DRM HLS rendition, same reasoning as
// Pluto3DS: the New 3DS's MVD hardware decoder has hard resolution/profile
// ceilings, so the smallest stream that plays cleanly beats a bigger one
// that doesn't decode at all.
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

Playback Catalog::resolve(const Entry& e) {
    error.clear();
    Playback p;
    std::string manifestUrl = e.url;   // channels arrive with this already filled in
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
    if (!r.ok()) { p.error="Playback unavailable ("+std::to_string(r.status)+")"; return p; }
    auto master = hls::parse(r.body);

    std::string finalUrl;
    if (!master.variants.empty()) {
        auto v = *std::min_element(master.variants.begin(), master.variants.end(),
                                   [](const hls::Variant& a,const hls::Variant& b){return a.bandwidth<b.bandwidth;});
        finalUrl = hls::resolve(manifestUrl, v.uri);
        if (!v.audio.empty()) {
            std::string best;
            for (const auto& a : master.audio) {
                if (a.groupId!=v.audio) continue;
                if (best.empty() || a.isDefault) best=a.uri;
            }
            if (!best.empty()) p.audioUrl = hls::resolve(manifestUrl, best);
        }
    } else if (!master.segments.empty()) {
        // manifestUrl was already a media (not master) playlist.
        finalUrl = manifestUrl;
    }
    if (finalUrl.empty()) { p.error="No playable video formats"; return p; }
    p.url = finalUrl;
    p.duration = e.duration;
    return p;
}
