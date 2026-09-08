#include "http.h"
#include "hls.h"
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
    if(R_SUCCEEDED(out.error)) out.error=httpcBeginRequest(&ctx);
    if(R_SUCCEEDED(out.error)) out.error=httpcGetResponseStatusCodeTimeout(&ctx,&out.status,15000000000ULL);
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
