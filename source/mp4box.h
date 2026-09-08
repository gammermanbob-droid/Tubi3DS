#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

// Minimal ISOBMFF (fragmented MP4 / CMAF) parsing -- just enough to pull
// H.264 (avcC) and AAC (esds) codec config out of an HLS "#EXT-X-MAP" init
// segment, and to walk a moof+mdat fragment's sample table into a flat list
// of {offset,size,timestamp} ready to hand to the existing TS-based decode
// path (processH264/processAAC in player.cpp). No exceptions; every
// function is defensive against truncated/malformed input (returns
// false/0/empty rather than reading out of bounds), since this parses
// untrusted network data on a 3DS with no crash-recovery to spare.
namespace mp4 {

struct TrackConfig {
    uint32_t trackId = 0;
    uint32_t timescale = 0;
    bool isVideo = false;
    bool isAudio = false;

    // Video (avcC). NALs stored WITHOUT startcodes -- caller prefixes
    // 00 00 00 01 when building an Annex-B buffer (see writeAnnexBNal).
    std::vector<std::vector<uint8_t>> spsList;
    std::vector<std::vector<uint8_t>> ppsList;
    uint8_t nalLengthSize = 4; // bytes per NAL length prefix in samples

    // Audio (esds / AudioSpecificConfig), enough to synthesize ADTS headers.
    uint8_t audioObjectType    = 2; // 2 = AAC-LC
    uint8_t audioFreqIndex     = 4; // MPEG-4 sampling_frequency_index (4 = 44100Hz)
    uint8_t audioChannelConfig = 2;

    // trex fallback defaults (moov/mvex), used when a fragment's tfhd/trun
    // don't specify a value themselves.
    uint32_t defaultSampleDuration = 0;
    uint32_t defaultSampleSize     = 0;
    uint32_t defaultSampleFlags    = 0;
};

struct InitInfo {
    std::vector<TrackConfig> tracks;
};

// Parses an init segment (ftyp+moov+...). Returns false if no moov / no
// usable (video or audio) track was found.
bool parseInit(const uint8_t* data, size_t size, InitInfo& out);

// One elementary-stream access unit, decoded from a moof+mdat fragment.
struct Sample {
    uint64_t offset = 0;  // absolute byte offset *within the fragment buffer*
    uint32_t size   = 0;
    uint64_t dts    = 0;  // decode timestamp, track timescale units (cumulative from tfdt)
    int64_t  cts    = 0;  // dts + composition offset, track timescale units
    bool keyframe   = false;
};

struct FragmentTrack {
    uint32_t trackId = 0;
    std::vector<Sample> samples;
};

struct FragmentInfo {
    std::vector<FragmentTrack> tracks;
};

// Parses every moof(+trun) in `data` (a single HLS fragment: normally one
// moof followed by one mdat, but this handles multiple pairs too) and
// resolves each sample's absolute byte offset within `data` itself, using
// the trun data-offset / tfhd base-data-offset / default-base-is-moof
// rules. `init` supplies each track's trex defaults so a fragment that
// omits duration/size/flags per-sample (common -- most packagers only send
// an explicit size per sample and inherit the rest from tfhd/trex) still
// resolves correctly. Returns false only if no moof at all was found.
bool parseFragment(const uint8_t* data, size_t size, const InitInfo& init, FragmentInfo& out);

// Converts one AVCC length-prefixed sample (raw bytes at sample/sampleSize,
// as referenced by a Sample) into Annex-B (each NAL prefixed with
// 00 00 00 01) into dst (caller-owned, capacity dstCap). Returns bytes
// written, or 0 if dstCap was too small or the sample is malformed.
size_t avccToAnnexB(const uint8_t* sample, uint32_t sampleSize, uint8_t nalLengthSize,
                     uint8_t* dst, size_t dstCap);

// Writes an Annex-B startcode (00 00 00 01) + NAL bytes into dst. Small
// helper used to prepend synthesized SPS/PPS from a TrackConfig ahead of
// the first video sample fed to processH264 (see player.cpp).
size_t writeAnnexBNal(const uint8_t* nal, size_t nalSize, uint8_t* dst, size_t dstCap);

// Synthesizes a 7-byte ADTS header (no CRC) for one raw AAC sample using
// the track's audioObjectType/audioFreqIndex/audioChannelConfig, and
// copies header+payload into dst. Returns total bytes written
// (7 + sampleSize), or 0 if dstCap was too small or sampleSize is too
// large to fit ADTS's 13-bit frame-length field.
size_t wrapAdts(const TrackConfig& track, const uint8_t* sample, uint32_t sampleSize,
                 uint8_t* dst, size_t dstCap);

} // namespace mp4
