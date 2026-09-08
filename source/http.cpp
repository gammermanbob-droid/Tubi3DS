#include "http.h"
#include "hls.h"
#include <map>
#include <cstdio>
#include <cstdarg>

// Scoped diagnostic for the cookie-jar addition below: logs, for requests
// to Tubi's live-manifest origin only (this is specifically about the
// Apollo-session 404 investigation, not a general request logger), whether
// a cookie was actually found+sent and whether one was actually captured
// off the response -- names only, never values, since a session cookie is
// exactly the kind of thing that shouldn't end up verbatim in a debug file
// someone pastes back into a chat. This settles conclusively whether
// libctru's httpc even exposes Set-Cookie at all (if every "captured=" line
// comes back empty, the jar has nothing to work with and the theory needs
// to move elsewhere) rather than us inferring it indirectly again.
static void httpDebugLog(const char* fmt, ...) {
    FILE* f = fopen("sdmc:/3ds/pluto3ds/http_debug.txt","a");
    if(!f) return;
    va_list args; va_start(args,fmt); vfprintf(f,fmt,args); va_end(args);
    fclose(f);
}
static std::string redactUrlForLog(const std::string& url) {
    auto q=url.find('?'); return q==std::string::npos ? url : url.substr(0,q)+"?<redacted>";
}
static std::string cookieNameOnly(const std::string& nameValue) {
    auto eq=nameValue.find('='); return eq==std::string::npos ? nameValue : nameValue.substr(0,eq);
}

// Minimal origin-scoped cookie jar. Nothing in this file previously read
// Set-Cookie or sent a Cookie header on any request. That's invisible for
// Tubi's VOD/on-demand playback and its plain catalog/EPG JSON endpoints,
// but live channels turned out to use server-side ad insertion (Tubi's
// "Apollo" session system -- see the #EXT-X-APOLLO-SESSION/-ROUTE tags in a
// live master playlist, and catalog_debug.txt's masterBody dumps): every
// fetch of a channel's playlist.m3u8 mints a brand-new Apollo session, and
// the per-bitrate child manifest URLs it returns are scoped to that exact
// session. Investigated via catalog_debug.txt across five separate fresh
// sessions (five different APOLLO-SESSION ids from five master re-fetches,
// each immediately followed by a child manifest request) -- every single
// one 404'd, which rules out staleness/timing and points at something
// structurally missing from the child request rather than a race. A cookie
// set on the master response and required on the child request is the most
// likely remaining candidate, since it's the one thing a real browser does
// automatically that this client has never done at all.
static std::map<std::string,std::string>& cookieJar() {
    static std::map<std::string,std::string> jar;
    return jar;
}
static std::string originOf(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme==std::string::npos) return url;
    auto pathStart = url.find('/', scheme+3);
    return pathStart==std::string::npos ? url : url.substr(0, pathStart);
}

static Response fetch(const std::string& url,const std::string& token,unsigned redirects) {
    Response out;
    if(url.rfind("https://",0)!=0) { out.error=-1; return out; }
    httpcContext ctx;
    out.error=httpcOpenContext(&ctx,HTTPC_METHOD_GET,url.c_str(),1);
    if(R_FAILED(out.error)) return out;
    // Keep TLS certificate verification enabled. Report compatibility failures.
    out.error=httpcAddRequestHeaderField(&ctx,"User-Agent","Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    if(R_SUCCEEDED(out.error)) out.error=httpcAddRequestHeaderField(&ctx,"Origin","https://tubitv.com");
    if(R_SUCCEEDED(out.error)) out.error=httpcAddRequestHeaderField(&ctx,"Referer","https://tubitv.com/");
    if(R_SUCCEEDED(out.error) && !token.empty()) out.error=httpcAddRequestHeaderField(&ctx,"Authorization",("Bearer "+token).c_str());
    bool isLiveManifest = originOf(url)=="https://live-manifest.production-public.tubi.io";
    bool cookieSent=false; std::string cookieSentName;
    {
        auto it = cookieJar().find(originOf(url));
        if (R_SUCCEEDED(out.error) && it != cookieJar().end() && !it->second.empty()) {
            out.error=httpcAddRequestHeaderField(&ctx,"Cookie",it->second.c_str());
            cookieSent=true; cookieSentName=cookieNameOnly(it->second);
        }
    }
    if(R_SUCCEEDED(out.error)) out.error=httpcBeginRequest(&ctx);
    if(R_SUCCEEDED(out.error)) out.error=httpcGetResponseStatusCodeTimeout(&ctx,&out.status,15000000000ULL);
    bool cookieCaptured=false; std::string cookieCapturedName;
    if(R_SUCCEEDED(out.error)) {
        // Capture Set-Cookie (if any) for later requests to this same
        // origin, regardless of status -- a redirect or even a 404 could
        // still be the response that hands out the session cookie a
        // following request needs. Only the "name=value" part is kept
        // (attributes like Path/Expires/HttpOnly/Secure after the first
        // ';' are meaningless on an outgoing Cookie header); a jar entry is
        // only ever replaced by a non-empty new value, never cleared by an
        // empty/missing Set-Cookie on some later unrelated request.
        char cookieBuf[2048]={};
        if (R_SUCCEEDED(httpcGetResponseHeader(&ctx,"Set-Cookie",cookieBuf,sizeof(cookieBuf))) && cookieBuf[0]) {
            std::string raw(cookieBuf);
            auto semi = raw.find(';');
            std::string nameValue = semi==std::string::npos ? raw : raw.substr(0,semi);
            if (!nameValue.empty()) {
                cookieJar()[originOf(url)] = nameValue;
                cookieCaptured=true; cookieCapturedName=cookieNameOnly(nameValue);
            }
        }
    }
    if (isLiveManifest) {
        httpDebugLog("GET %s status=%lu cookieSent=%s captured=%s\n",
                     redactUrlForLog(url).c_str(), (unsigned long)out.status,
                     cookieSent ? cookieSentName.c_str() : "(none)",
                     cookieCaptured ? cookieCapturedName.c_str() : "(none)");
    }
    if(R_SUCCEEDED(out.error) && out.status==200) {
        u32 before=0,total=0; httpcGetDownloadSizeState(&ctx,&before,&total);
        do {
            u8 buf[8192]; out.error=httpcReceiveDataTimeout(&ctx,buf,sizeof(buf),15000000000ULL);
            u32 after=before; Result sizeResult=httpcGetDownloadSizeState(&ctx,&after,&total);
            if(R_FAILED(sizeResult) || after<before || after-before>sizeof(buf)) { out.error=-2; break; }
            out.body.append(reinterpret_cast<char*>(buf),after-before); before=after;
            if(out.body.size()>8*1024*1024) { out.error=-3; break; }
        } while(out.error==(Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    }
    std::string location;
    if(R_SUCCEEDED(out.error) && redirects<4 &&
       (out.status==301 || out.status==302 || out.status==303 || out.status==307 || out.status==308)) {
        char target[8192]={};
        if(R_SUCCEEDED(httpcGetResponseHeader(&ctx,"Location",target,sizeof(target)))) location=hls::resolve(url,target);
    }
    httpcCancelConnection(&ctx); httpcCloseContext(&ctx);
    if(!location.empty()) {
        // Never forward the bearer header to a different origin.
        auto origin=[](const std::string& u){ auto p=u.find("://");return u.substr(0,u.find('/',p+3)); };
        if(location.rfind("https://",0)==0 && (token.empty() || origin(location)==origin(url))) return fetch(location,token,redirects+1);
    }
    return out;
}

Response get(const std::string& url,const std::string& token) { return fetch(url,token,0); }
