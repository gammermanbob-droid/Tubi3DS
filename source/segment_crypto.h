#pragma once
#include <array>
#include <string>
#include <cstdint>
extern "C" {
#include "aes.h"
}
namespace hls {
inline bool parseIV(const std::string& text,uint64_t sequence,std::array<uint8_t,16>& iv) {
    iv.fill(0);
    if(text.empty()) { for(int i=15;i>=8;--i) { iv[i]=sequence&255; sequence>>=8; } return true; }
    if(text.rfind("0x",0)!=0 && text.rfind("0X",0)!=0) return false;
    auto digits=text.substr(2); if(digits.empty() || digits.size()>32) return false;
    unsigned nibble=0;
    for(auto i=digits.rbegin();i!=digits.rend();++i,++nibble) {
        int v=*i>='0'&&*i<='9'?*i-'0':*i>='a'&&*i<='f'?*i-'a'+10:*i>='A'&&*i<='F'?*i-'A'+10:-1;
        if(v<0) return false;
        iv[15-nibble/2]|=v<<((nibble%2)*4);
    }
    return true;
}
inline bool decryptSegment(std::string& bytes,const std::string& key,const std::array<uint8_t,16>& iv) {
    if(key.size()!=16 || bytes.empty() || bytes.size()%16) return false;
    AES_ctx ctx;
    AES_init_ctx_iv(&ctx,reinterpret_cast<const uint8_t*>(key.data()),iv.data());
    AES_CBC_decrypt_buffer(&ctx,reinterpret_cast<uint8_t*>(&bytes[0]),bytes.size());
    unsigned pad=static_cast<uint8_t>(bytes.back());
    if(pad<1 || pad>16 || pad>bytes.size()) return false;
    for(size_t i=bytes.size()-pad;i<bytes.size();++i) if(static_cast<uint8_t>(bytes[i])!=pad) return false;
    bytes.resize(bytes.size()-pad); return true;
}
inline bool validTransportStream(const std::string& bytes) {
    if(bytes.empty() || bytes.size()%188) return false;
    for(size_t i=0;i<bytes.size();i+=188) if(static_cast<uint8_t>(bytes[i])!=0x47) return false;
    return true;
}
}
