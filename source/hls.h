#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdint>
#include <cstdlib>

namespace hls {
struct Variant { std::string uri, codecs, audio, subtitles; unsigned bandwidth=0, width=0, height=0; };
struct Segment { std::string uri, method, key, iv; uint64_t sequence=0; double duration=0; bool discontinuity=false; };
// A #EXT-X-MEDIA:TYPE=AUDIO rendition: groupId matches a Variant's `audio`
// attribute, uri is the separate audio-only media playlist for that group.
struct AudioRendition { std::string groupId, uri; bool isDefault=false; };
// A #EXT-X-MEDIA:TYPE=SUBTITLES rendition: groupId matches a Variant's
// `subtitles` attribute, uri is a WebVTT media playlist (a list of .vtt
// segments, not muxed into the TS stream) for that group/language.
struct SubtitleRendition { std::string groupId, uri, language; bool isDefault=false, isForced=false; };
struct Playlist { std::vector<Variant> variants; std::vector<Segment> segments; std::vector<AudioRendition> audio; std::vector<SubtitleRendition> subtitles; bool end=false, fragmented=false; std::string initSegmentUri; };
inline std::map<std::string,std::string> attributes(const std::string& text) {
    std::map<std::string,std::string> out;
    size_t p=0;
    while (p<text.size()) {
        while(p<text.size() && (text[p]==',' || text[p]==' ')) ++p;
        auto eq=text.find('=',p); if(eq==std::string::npos) break;
        std::string key=text.substr(p,eq-p), value; p=eq+1;
        if(p<text.size() && text[p]=='"') {
            auto end=text.find('"',++p); if(end==std::string::npos) break;
            value=text.substr(p,end-p); p=end+1;
        } else {
            auto end=text.find(',',p); if(end==std::string::npos) end=text.size();
            value=text.substr(p,end-p); p=end;
        }
        out[key]=value;
    }
    return out;
}
inline Playlist parse(const std::string& body) {
    Playlist out; std::istringstream lines(body); std::string line;
    Variant variant; Segment segment; bool pending=false, discontinuity=false;
    uint64_t sequence=0;
    while(std::getline(lines,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) continue;
        if(line.rfind("#EXT-X-STREAM-INF:",0)==0) {
            auto a=attributes(line.substr(18)); variant={};
            variant.bandwidth=std::strtoul(a["BANDWIDTH"].c_str(),nullptr,10);
            variant.codecs=a["CODECS"]; variant.audio=a["AUDIO"]; variant.subtitles=a["SUBTITLES"];
            auto x=a["RESOLUTION"].find('x');
            if(x!=std::string::npos) { variant.width=std::strtoul(a["RESOLUTION"].c_str(),nullptr,10); variant.height=std::strtoul(a["RESOLUTION"].c_str()+x+1,nullptr,10); }
            pending=true;
        } else if(line.rfind("#EXT-X-MEDIA-SEQUENCE:",0)==0) sequence=std::strtoull(line.c_str()+22,nullptr,10);
        else if(line.rfind("#EXTINF:",0)==0) segment.duration=std::strtod(line.c_str()+8,nullptr);
        else if(line.rfind("#EXT-X-KEY:",0)==0) { auto a=attributes(line.substr(11)); segment.method=a["METHOD"]; segment.key=a["URI"]; segment.iv=a["IV"]; }
        else if(line.rfind("#EXT-X-MEDIA:",0)==0) {
            auto a=attributes(line.substr(13));
            if(a["TYPE"]=="AUDIO" && !a["URI"].empty()) out.audio.push_back({a["GROUP-ID"],a["URI"],a["DEFAULT"]=="YES"});
            else if(a["TYPE"]=="SUBTITLES" && !a["URI"].empty())
                out.subtitles.push_back({a["GROUP-ID"],a["URI"],a["LANGUAGE"],a["DEFAULT"]=="YES",a["FORCED"]=="YES"});
        }
        else if(line=="#EXT-X-DISCONTINUITY") discontinuity=true;
        else if(line=="#EXT-X-ENDLIST") out.end=true;
        else if(line.rfind("#EXT-X-MAP:",0)==0) { auto a=attributes(line.substr(11)); out.fragmented=true; out.initSegmentUri=a["URI"]; }
        else if(line[0]!='#') {
            if(pending) { variant.uri=line; out.variants.push_back(variant); pending=false; }
            else { segment.uri=line; segment.sequence=sequence++; segment.discontinuity=discontinuity; out.segments.push_back(segment); discontinuity=false; }
        }
    }
    return out;
}
inline std::string resolve(const std::string& base,const std::string& ref) {
    if(ref.rfind("https://",0)==0 || ref.rfind("http://",0)==0) return ref;
    auto scheme=base.find("://"); if(scheme==std::string::npos) return "";
    if(ref.rfind("//",0)==0) return base.substr(0,scheme+1)+ref;
    auto path=base.find('/',scheme+3); auto origin=base.substr(0,path);
    if(!ref.empty() && ref[0]=='/') return origin+ref;
    auto clean=base.substr(0,base.find_first_of("?#"));
    if(!ref.empty() && ref[0]=='?') return clean+ref;
    return (path==std::string::npos?origin+"/":clean.substr(0,clean.rfind('/')+1))+ref;
}
}
