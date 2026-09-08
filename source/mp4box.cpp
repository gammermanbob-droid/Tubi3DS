#include "mp4box.h"
#include <cstring>

namespace mp4 {

namespace {

// A single box's header, plus where its payload starts/ends within the
// buffer that was walked (all offsets are absolute into that buffer).
struct Box {
    uint32_t type = 0; // fourcc, big-endian bytes packed into a u32 for cheap compares
    uint64_t bodyStart = 0;
    uint64_t bodyEnd   = 0; // exclusive
};

inline uint32_t fourcc(char a, char b, char c, char d) {
    return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
           (uint32_t(uint8_t(c)) << 8)  |  uint32_t(uint8_t(d));
}

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t rd64(const uint8_t* p) {
    return (uint64_t(rd32(p)) << 32) | uint64_t(rd32(p + 4));
}
inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Reads one box header at `pos` (absolute offset into `data`, `size` total
// bytes available). On success returns true and fills `box` with the
// fourcc plus the payload's [start,end) range, and advances `pos` past the
// whole box (header+payload) so the caller can keep walking. Handles the
// 64-bit "largesize" extension (size field == 1) and the "box extends to
// end of buffer" case (size field == 0). Returns false (leaving `pos`
// unchanged) if fewer than 8 bytes remain or the declared size is
// inconsistent with the buffer -- callers must stop walking in that case
// rather than loop forever on malformed input.
bool readBoxAt(const uint8_t* data, size_t size, size_t& pos, Box& box) {
    if (pos + 8 > size) return false;
    uint64_t declared = rd32(data + pos);
    uint32_t type = rd32(data + pos + 4);
    size_t hdr = 8;
    if (declared == 1) {
        if (pos + 16 > size) return false;
        declared = rd64(data + pos + 8);
        hdr = 16;
    } else if (declared == 0) {
        declared = size - pos;
    }
    if (declared < hdr || pos + declared > size) return false;
    box.type = type;
    box.bodyStart = pos + hdr;
    box.bodyEnd = pos + declared;
    pos += declared;
    return true;
}

// Finds the first direct child box of the given fourcc within
// [start,end). Does not recurse.
bool findChild(const uint8_t* data, size_t size, uint64_t start, uint64_t end,
               uint32_t type, Box& out) {
    size_t p = (size_t)start;
    size_t e = (size_t)end;
    if (e > size) e = size;
    while (p < e) {
        Box b;
        size_t before = p;
        if (!readBoxAt(data, e, p, b)) break;
        if (p <= before) break; // safety: never spin on a zero-progress box
        if (b.type == type) { out = b; return true; }
    }
    return false;
}

// Calls fn(Box) for every direct child box within [start,end).
template <typename Fn>
void forEachChild(const uint8_t* data, size_t size, uint64_t start, uint64_t end, Fn fn) {
    size_t p = (size_t)start;
    size_t e = (size_t)end;
    if (e > size) e = size;
    while (p < e) {
        Box b;
        size_t before = p;
        if (!readBoxAt(data, e, p, b)) break;
        if (p <= before) break;
        fn(b);
    }
}

// MPEG-4 "expandable" descriptor length (used inside esds): up to 4 bytes,
// each contributing 7 bits, continuation signaled by the top bit.
uint32_t readDescLen(const uint8_t* data, size_t size, size_t& p) {
    uint32_t val = 0;
    for (int i = 0; i < 4 && p < size; ++i) {
        uint8_t b = data[p++];
        val = (val << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return val;
}

// Descends stsd -> (avc1|mp4a|...) sample entry -> avcC/esds, filling in
// video or audio config on `track`. `stsdBodyStart/End` is the stsd box's
// payload range (i.e. *after* stsd's own 8-byte version/flags+entry-count
// header has already been skipped by the caller).
void parseSampleEntry(const uint8_t* data, size_t size, uint64_t entryStart, uint64_t entryEnd,
                       bool isVideo, TrackConfig& track) {
    // Every SampleEntry (ISO/IEC 14496-12 8.5.2) starts with:
    //   6 bytes reserved, 2 bytes data_reference_index
    // VisualSampleEntry then has a further 70 bytes of fixed fields before
    // any nested boxes (avcC etc); AudioSampleEntry has a further 20 bytes.
    const uint64_t commonHdr = 8;
    if (entryStart + commonHdr > entryEnd) return;
    uint64_t fixedLen = isVideo ? (commonHdr + 70) : (commonHdr + 20);
    if (entryStart + fixedLen > entryEnd) return;
    uint64_t nestedStart = entryStart + fixedLen;

    if (isVideo) {
        Box avcC;
        if (findChild(data, size, nestedStart, entryEnd, fourcc('a','v','c','C'), avcC)) {
            const uint8_t* p = data + avcC.bodyStart;
            size_t n = (size_t)(avcC.bodyEnd - avcC.bodyStart);
            // configurationVersion(1) AVCProfileIndication(1) profile_compat(1)
            // AVCLevelIndication(1) then 6+2 bits: reserved/lengthSizeMinusOne
            if (n < 6) return;
            track.nalLengthSize = (p[4] & 0x03) + 1;
            size_t off = 5;
            uint8_t numSps = p[off++] & 0x1f;
            for (uint8_t i = 0; i < numSps && off + 2 <= n; ++i) {
                uint16_t len = rd16(p + off); off += 2;
                if (off + len > n) break;
                track.spsList.emplace_back(p + off, p + off + len);
                off += len;
            }
            if (off >= n) return;
            uint8_t numPps = p[off++];
            for (uint8_t i = 0; i < numPps && off + 2 <= n; ++i) {
                uint16_t len = rd16(p + off); off += 2;
                if (off + len > n) break;
                track.ppsList.emplace_back(p + off, p + off + len);
                off += len;
            }
        }
    } else {
        Box esds;
        if (findChild(data, size, nestedStart, entryEnd, fourcc('e','s','d','s'), esds)) {
            const uint8_t* p = data + esds.bodyStart;
            size_t n = (size_t)(esds.bodyEnd - esds.bodyStart);
            size_t off = 4; // version+flags
            if (off >= n || p[off] != 0x03) return; // ES_DescrTag
            ++off;
            readDescLen(p, n, off);
            if (off + 3 > n) return;
            off += 2; // ES_ID
            ++off;    // flags byte
            if (off >= n || p[off] != 0x04) return; // DecoderConfigDescrTag
            ++off;
            uint32_t dcLen = readDescLen(p, n, off);
            size_t dcEnd = off + dcLen;
            if (dcEnd > n) dcEnd = n;
            if (off + 13 > dcEnd) return;
            off += 13; // objectTypeIndication, streamType/upStream/reserved,
                       // bufferSizeDB(3), maxBitrate(4), avgBitrate(4)
            if (off >= dcEnd || p[off] != 0x05) return; // DecSpecificInfoTag
            ++off;
            uint32_t asLen = readDescLen(p, n, off);
            if (asLen < 2 || off + 2 > n) return;
            (void)asLen;
            uint8_t b0 = p[off], b1 = p[off + 1];
            track.audioObjectType    = (b0 >> 3) & 0x1f;
            track.audioFreqIndex     = uint8_t(((b0 & 0x7) << 1) | (b1 >> 7));
            track.audioChannelConfig = (b1 >> 3) & 0xf;
        }
    }
}

void parseTrak(const uint8_t* data, size_t size, uint64_t trakStart, uint64_t trakEnd,
               InitInfo& out) {
    Box tkhd;
    if (!findChild(data, size, trakStart, trakEnd, fourcc('t','k','h','d'), tkhd)) return;
    if (tkhd.bodyEnd - tkhd.bodyStart < 1) return;

    Box mdia;
    if (!findChild(data, size, trakStart, trakEnd, fourcc('m','d','i','a'), mdia)) return;

    Box mdhd;
    if (!findChild(data, size, mdia.bodyStart, mdia.bodyEnd, fourcc('m','d','h','d'), mdhd)) return;

    Box hdlr;
    if (!findChild(data, size, mdia.bodyStart, mdia.bodyEnd, fourcc('h','d','l','r'), hdlr)) return;

    // hdlr: version(1) flags(3) pre_defined(4) handler_type(4) ...
    const uint8_t* hp = data + hdlr.bodyStart;
    size_t hn = (size_t)(hdlr.bodyEnd - hdlr.bodyStart);
    if (hn < 12) return;
    uint32_t handlerType = rd32(hp + 8);
    bool isVideo = handlerType == fourcc('v','i','d','e');
    bool isAudio = handlerType == fourcc('s','o','u','n');
    if (!isVideo && !isAudio) return;

    TrackConfig track;
    track.isVideo = isVideo;
    track.isAudio = isAudio;

    // tkhd: version-dependent layout. version==1 uses 64-bit
    // creation/modification times; track_ID follows either way.
    {
        const uint8_t* p = data + tkhd.bodyStart;
        size_t n = (size_t)(tkhd.bodyEnd - tkhd.bodyStart);
        uint8_t ver = p[0];
        size_t idOff = (ver == 1) ? 20 : 12;
        if (idOff + 4 > n) return;
        track.trackId = rd32(p + idOff);
    }

    // mdhd: version-dependent; timescale is a u32 either way, just at a
    // different offset.
    {
        const uint8_t* p = data + mdhd.bodyStart;
        size_t n = (size_t)(mdhd.bodyEnd - mdhd.bodyStart);
        uint8_t ver = p[0];
        size_t tsOff = (ver == 1) ? 20 : 12;
        if (tsOff + 4 > n) return;
        track.timescale = rd32(p + tsOff);
    }

    Box minf;
    if (findChild(data, size, mdia.bodyStart, mdia.bodyEnd, fourcc('m','i','n','f'), minf)) {
        Box stbl;
        if (findChild(data, size, minf.bodyStart, minf.bodyEnd, fourcc('s','t','b','l'), stbl)) {
            Box stsd;
            if (findChild(data, size, stbl.bodyStart, stbl.bodyEnd, fourcc('s','t','s','d'), stsd)) {
                // stsd: version(1) flags(3) entry_count(4) then entries.
                size_t sn = (size_t)(stsd.bodyEnd - stsd.bodyStart);
                if (sn >= 8) {
                    uint64_t entriesStart = stsd.bodyStart + 8;
                    size_t p = (size_t)entriesStart;
                    Box entry;
                    if (readBoxAt(data, size, p, entry)) {
                        // entry.bodyStart/End here is the sample entry's own
                        // header-exclusive range -- but readBoxAt already
                        // gave us bodyStart pointing past the entry's own
                        // 8-byte box header, which is exactly the "entryStart"
                        // parseSampleEntry expects (SampleEntry's own fields
                        // start right there).
                        parseSampleEntry(data, size, entry.bodyStart, entry.bodyEnd, isVideo, track);
                    }
                }
            }
        }
    }

    out.tracks.push_back(std::move(track));
}

} // namespace

bool parseInit(const uint8_t* data, size_t size, InitInfo& out) {
    out.tracks.clear();
    Box moov;
    if (!findChild(data, size, 0, size, fourcc('m','o','o','v'), moov)) return false;

    forEachChild(data, size, moov.bodyStart, moov.bodyEnd, [&](const Box& b) {
        if (b.type == fourcc('t','r','a','k')) parseTrak(data, size, b.bodyStart, b.bodyEnd, out);
    });

    // mvex/trex: per-track defaults used when a fragment's tfhd/trun omit
    // a field. Matched up to the tracks we already collected by trackId.
    Box mvex;
    if (findChild(data, size, moov.bodyStart, moov.bodyEnd, fourcc('m','v','e','x'), mvex)) {
        forEachChild(data, size, mvex.bodyStart, mvex.bodyEnd, [&](const Box& b) {
            if (b.type != fourcc('t','r','e','x')) return;
            const uint8_t* p = data + b.bodyStart;
            size_t n = (size_t)(b.bodyEnd - b.bodyStart);
            if (n < 24) return; // version/flags(4) track_ID(4) + 4x u32
            uint32_t trackId = rd32(p + 4);
            uint32_t defDuration = rd32(p + 8);
            uint32_t defSize     = rd32(p + 12);
            uint32_t defFlags    = rd32(p + 16);
            for (auto& t : out.tracks) {
                if (t.trackId == trackId) {
                    t.defaultSampleDuration = defDuration;
                    t.defaultSampleSize     = defSize;
                    t.defaultSampleFlags    = defFlags;
                }
            }
        });
    }

    return !out.tracks.empty();
}

namespace {

const TrackConfig* findTrack(const InitInfo& init, uint32_t trackId) {
    for (auto& t : init.tracks) if (t.trackId == trackId) return &t;
    return nullptr;
}

// Parses one traf's tfhd+tfdt+trun into Sample entries appended to `out`,
// and returns the track_ID it belongs to (0 if malformed/unrecognized).
uint32_t parseTraf(const uint8_t* data, size_t size, uint64_t trafStart, uint64_t trafEnd,
                    uint64_t moofStart, const InitInfo& init, std::vector<Sample>& out) {
    Box tfhd;
    if (!findChild(data, size, trafStart, trafEnd, fourcc('t','f','h','d'), tfhd)) return 0;
    const uint8_t* tp = data + tfhd.bodyStart;
    size_t tn = (size_t)(tfhd.bodyEnd - tfhd.bodyStart);
    if (tn < 8) return 0;
    uint32_t tfhdFlags = rd32(tp) & 0x00ffffff;
    uint32_t trackId = rd32(tp + 4);

    uint64_t baseDataOffset = moofStart; // default-base-is-moof is the CMAF norm
    uint32_t defSampleDuration = 0, defSampleSize = 0, defSampleFlags = 0;
    bool haveTfhdDuration = false, haveTfhdSize = false, haveTfhdFlags = false;
    {
        size_t off = 8;
        if (tfhdFlags & 0x000001) { // base-data-offset-present
            if (off + 8 > tn) return 0;
            baseDataOffset = rd64(tp + off); off += 8;
        }
        if (tfhdFlags & 0x000002) { // sample-description-index-present
            if (off + 4 > tn) return 0;
            off += 4;
        }
        if (tfhdFlags & 0x000008) { // default-sample-duration-present
            if (off + 4 > tn) return 0;
            defSampleDuration = rd32(tp + off); off += 4; haveTfhdDuration = true;
        }
        if (tfhdFlags & 0x000010) { // default-sample-size-present
            if (off + 4 > tn) return 0;
            defSampleSize = rd32(tp + off); off += 4; haveTfhdSize = true;
        }
        if (tfhdFlags & 0x000020) { // default-sample-flags-present
            if (off + 4 > tn) return 0;
            defSampleFlags = rd32(tp + off); off += 4; haveTfhdFlags = true;
        }
    }

    const TrackConfig* cfg = findTrack(init, trackId);
    if (!haveTfhdDuration) defSampleDuration = cfg ? cfg->defaultSampleDuration : 0;
    if (!haveTfhdSize)     defSampleSize     = cfg ? cfg->defaultSampleSize     : 0;
    if (!haveTfhdFlags)    defSampleFlags    = cfg ? cfg->defaultSampleFlags    : 0;

    uint64_t baseMediaDecodeTime = 0;
    Box tfdt;
    if (findChild(data, size, trafStart, trafEnd, fourcc('t','f','d','t'), tfdt)) {
        const uint8_t* p = data + tfdt.bodyStart;
        size_t n = (size_t)(tfdt.bodyEnd - tfdt.bodyStart);
        if (n >= 1) {
            uint8_t ver = p[0];
            if (ver == 1 && n >= 12) baseMediaDecodeTime = rd64(p + 4);
            else if (ver == 0 && n >= 8) baseMediaDecodeTime = rd32(p + 4);
        }
    }

    uint64_t cursor = baseMediaDecodeTime;
    // A traf may (rarely) contain more than one trun; each trun's
    // data-offset is independent (relative to baseDataOffset), and sample
    // timing continues to accumulate across them in document order.
    bool any = false;
    forEachChild(data, size, trafStart, trafEnd, [&](const Box& b) {
        if (b.type != fourcc('t','r','u','n')) return;
        any = true;
        const uint8_t* rp = data + b.bodyStart;
        size_t rn = (size_t)(b.bodyEnd - b.bodyStart);
        if (rn < 8) return;
        uint32_t runFlags = rd32(rp) & 0x00ffffff;
        uint32_t sampleCount = rd32(rp + 4);
        size_t off = 8;
        int64_t dataOffset = 0;
        if (runFlags & 0x000001) { // data-offset-present
            if (off + 4 > rn) return;
            dataOffset = int32_t(rd32(rp + off)); off += 4;
        }
        uint32_t firstSampleFlags = defSampleFlags;
        bool haveFirstFlags = false;
        if (runFlags & 0x000004) { // first-sample-flags-present
            if (off + 4 > rn) return;
            firstSampleFlags = rd32(rp + off); off += 4; haveFirstFlags = true;
        }
        uint64_t sampleCursor = baseDataOffset + (uint64_t)(int64_t)dataOffset;
        for (uint32_t i = 0; i < sampleCount; ++i) {
            uint32_t duration = defSampleDuration;
            uint32_t sz = defSampleSize;
            uint32_t flags = (i == 0 && haveFirstFlags) ? firstSampleFlags : defSampleFlags;
            int32_t cto = 0;
            if (runFlags & 0x100) { if (off + 4 > rn) break; duration = rd32(rp + off); off += 4; }
            if (runFlags & 0x200) { if (off + 4 > rn) break; sz = rd32(rp + off); off += 4; }
            if (runFlags & 0x400) { if (off + 4 > rn) break; flags = rd32(rp + off); off += 4; }
            if (runFlags & 0x800) { if (off + 4 > rn) break; cto = int32_t(rd32(rp + off)); off += 4; }

            Sample s;
            s.offset = sampleCursor;
            s.size = sz;
            s.dts = cursor;
            s.cts = (int64_t)cursor + cto;
            // sample_is_non_sync_sample is bit 16 (0-based, from the LSB) of
            // the 32-bit sample_flags word (ISO/IEC 14496-12 8.8.3.1). Clear
            // == sync sample == keyframe.
            s.keyframe = ((flags >> 16) & 0x1) == 0;
            out.push_back(s);

            sampleCursor += sz;
            cursor += duration;
        }
    });
    (void)any;
    return trackId;
}

} // namespace

bool parseFragment(const uint8_t* data, size_t size, const InitInfo& init, FragmentInfo& out) {
    out.tracks.clear();
    bool foundAny = false;
    size_t p = 0;
    while (p < size) {
        Box b;
        size_t before = p;
        if (!readBoxAt(data, size, p, b)) break;
        if (p <= before) break;
        if (b.type != fourcc('m','o','o','f')) continue;
        foundAny = true;
        uint64_t moofStart = before;

        forEachChild(data, size, b.bodyStart, b.bodyEnd, [&](const Box& tb) {
            if (tb.type != fourcc('t','r','a','f')) return;
            std::vector<Sample> samples;
            uint32_t trackId = parseTraf(data, size, tb.bodyStart, tb.bodyEnd, moofStart, init, samples);
            if (trackId == 0 || samples.empty()) return;
            // Drop any sample whose byte range doesn't actually fit in this
            // buffer -- a malformed/truncated fragment must never hand the
            // caller an out-of-bounds range.
            std::vector<Sample> valid;
            valid.reserve(samples.size());
            for (auto& s : samples) {
                if (s.offset <= size && s.size <= size - s.offset) valid.push_back(s);
            }
            if (valid.empty()) return;
            FragmentTrack* track = nullptr;
            for (auto& t : out.tracks) if (t.trackId == trackId) { track = &t; break; }
            if (!track) { out.tracks.push_back({trackId, {}}); track = &out.tracks.back(); }
            track->samples.insert(track->samples.end(), valid.begin(), valid.end());
        });
    }
    return foundAny;
}

size_t writeAnnexBNal(const uint8_t* nal, size_t nalSize, uint8_t* dst, size_t dstCap) {
    if (dstCap < nalSize + 4) return 0;
    dst[0] = 0; dst[1] = 0; dst[2] = 0; dst[3] = 1;
    if (nalSize) memcpy(dst + 4, nal, nalSize);
    return nalSize + 4;
}

size_t avccToAnnexB(const uint8_t* sample, uint32_t sampleSize, uint8_t nalLengthSize,
                     uint8_t* dst, size_t dstCap) {
    if (nalLengthSize < 1 || nalLengthSize > 4) return 0;
    size_t out = 0;
    size_t p = 0;
    while (p + nalLengthSize <= sampleSize) {
        uint32_t len = 0;
        for (uint8_t i = 0; i < nalLengthSize; ++i) len = (len << 8) | sample[p + i];
        p += nalLengthSize;
        if (len == 0 || p + len > sampleSize) break; // malformed/truncated -- stop, keep what we have
        size_t w = writeAnnexBNal(sample + p, len, dst + out, dstCap - out);
        if (w == 0) return 0; // ran out of room
        out += w;
        p += len;
    }
    return out;
}

size_t wrapAdts(const TrackConfig& track, const uint8_t* sample, uint32_t sampleSize,
                 uint8_t* dst, size_t dstCap) {
    // ADTS frame length field is 13 bits and includes the 7-byte header.
    uint32_t frameLen = sampleSize + 7;
    if (frameLen > 0x1FFF) return 0;
    if (dstCap < frameLen) return 0;

    // ADTS profile field = MPEG-4 AudioObjectType - 1 for the four values
    // (Main/LC/SSR/LTP) ADTS can express; anything else (e.g. HE-AAC's
    // AOT 5) falls back to LC (1), which is what nearly every AAC decoder
    // -- including the one this app already drives via processAAC --
    // expects to see signaled regardless of the "real" extension AOT.
    uint8_t aot = track.audioObjectType;
    uint8_t profile = (aot >= 1 && aot <= 4) ? (aot - 1) : 1;
    uint8_t freqIdx = track.audioFreqIndex & 0xf;
    uint8_t chanCfg = track.audioChannelConfig & 0x7;

    dst[0] = 0xFF;
    dst[1] = 0xF1; // MPEG-4, no CRC
    dst[2] = uint8_t((profile << 6) | (freqIdx << 2) | (chanCfg >> 2));
    dst[3] = uint8_t(((chanCfg & 0x3) << 6) | ((frameLen >> 11) & 0x3));
    dst[4] = uint8_t((frameLen >> 3) & 0xFF);
    dst[5] = uint8_t(((frameLen & 0x7) << 5) | 0x1F);
    dst[6] = 0xFC;
    if (sampleSize) memcpy(dst + 7, sample, sampleSize);
    return frameLen;
}

} // namespace mp4
