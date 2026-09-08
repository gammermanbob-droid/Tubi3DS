#pragma once
#include <string>
#include <functional>
#include <utility>
// Optional bottom-screen guide while live video is running.
extern std::function<void()> playerGuideDraw;
extern std::function<bool()> playerGuideInput;
extern std::function<void()> playerGuideCleanup;

// Optional: called by the download thread(s) when a live channel's playlist
// keeps failing to refresh (most likely an expired Pluto session token) or,
// proactively, on a timer as a safety net for long-running sessions. Returns
// {videoPlaylistUrl, audioPlaylistUrl} (audioPlaylistUrl empty when the
// channel has no separate audio track). Each download thread reads only the
// member it owns. Either member empty means a fresh URL couldn't be
// obtained for that thread. Not used for on-demand playback, whose playlist
// already carries an end tag and is downloaded once. Set/cleared by the
// caller around playerPlay(), same as the playerGuide* callbacks above.
extern std::function<std::pair<std::string,std::string>()> playerRenewUrl;

// Plays a Jellyfin MPEG-TS/H.264 stream using the New 3DS MVD hardware decoder.
// Blocks until the stream ends or the user presses B.
// runTimeTicks is the item's total duration (10,000,000 ticks/sec, 0 if unknown)
// and drives the bottom-screen seek bar. series/title/year are shown above it.
// startSec is the resume offset (seconds) the stream was seeked to server-side;
// it's added to the PTS-derived position so the seek bar reads the true time.
// A live transcode stream can't be seeked client-side, so D-Pad Left/Right
// (-10s/+30s) end playback and report the target via *seekOut (-1 = no seek);
// the caller then requests a fresh stream at that position and calls back in.
// Returns false if MVD init fails (Old 3DS or allocation error).
bool playerPlay(const std::string& url, long long runTimeTicks = 0,
                const std::string& series = "",
                const std::string& title  = "",
                int year = 0,
                double startSec = 0.0,
                double* seekOut = nullptr,
                const std::string& subtitleVtt = "",
                bool* finishedOut = nullptr,
                const std::string& artworkData = "",
                bool audioOnly = false,
                // Separate audio-only playlist URL for a demuxed HLS channel
                // (e.g. Bloomberg TV+); empty for ordinary muxed audio/video.
                // Downloaded and decoded on its own thread/ring alongside the
                // video from `url`, synchronized by PES PTS the same way the
                // video pacer already syncs to the DSP audio clock.
                const std::string& audioUrl = "",
                // Catalog::resolve()'s trace of how it looked for subtitles
                // for this title (see Playback::subtitleDebug) — written into
                // player_debug.txt verbatim so a "no subtitles" report can be
                // diagnosed from the log alone.
                const std::string& subtitleDebug = "");
