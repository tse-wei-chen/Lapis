#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "lapis/media_runtime.h"

extern "C" {
#include <libavutil/dict.h>
}

#include <cstddef>
#include <cstdint>
#include <iostream>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

int main() {
    using lapis::media::SubtitleTrackKind;
    using lapis::media::ffmpeg::FfmpegDemuxer;

    // In-memory FFmpeg stream descriptors: no subprocess, device or subtitle
    // decoding is needed to test track identity / codec classification.
    AVFormatContext* context = avformat_alloc_context();
    CHECK(context != nullptr);
    auto* video = avformat_new_stream(context, nullptr);
    CHECK(video != nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    auto* text = avformat_new_stream(context, nullptr);
    CHECK(text != nullptr);
    text->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    text->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
    CHECK(av_dict_set(&text->metadata, "language", "jpn", 0) == 0);

    auto* bitmap = avformat_new_stream(context, nullptr);
    CHECK(bitmap != nullptr);
    bitmap->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    bitmap->codecpar->codec_id = AV_CODEC_ID_HDMV_PGS_SUBTITLE;
    CHECK(av_dict_set(&bitmap->metadata, "language", "zho", 0) == 0);

    const auto inventory = FfmpegDemuxer::inspect_subtitle_streams(*context);
    CHECK(inventory.size() == 2);
    CHECK(inventory[0].stream_index == 1);
    CHECK(inventory[0].kind == SubtitleTrackKind::Text);
    CHECK(inventory[0].language == "jpn");
    CHECK(!inventory[0].codec_name.empty());
    CHECK(inventory[1].stream_index == 2);
    CHECK(inventory[1].kind == SubtitleTrackKind::Bitmap);
    CHECK(inventory[1].language == "zho");
    avformat_free_context(context);

    CHECK(lapis_runtime_get_abi_version() == LAPIS_MEDIA_RUNTIME_ABI_VERSION);
    CHECK(lapis_runtime_get_subtitle_tracks_api_version() ==
          LAPIS_SUBTITLE_TRACKS_API_VERSION);
    CHECK(sizeof(lapis_subtitle_track_info) == 72);
    CHECK(offsetof(lapis_subtitle_track_info, codec_name_utf8) == 32);

    lapis_runtime* runtime = nullptr;
    CHECK(lapis_runtime_create(&runtime) == LAPIS_OK);
    lapis_session* session = nullptr;
    lapis_playback_session_id id{};
    id.bytes[0] = 7;
    CHECK(lapis_session_create(runtime, id, &session) == LAPIS_OK);

    std::uint32_t count = 99;
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{0}, nullptr, 0, &count) ==
          LAPIS_E_INVALID_STATE);
    CHECK(count == 0);
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{0}, nullptr, 1, &count) ==
          LAPIS_E_INVALID_ARGUMENT);

    const lapis_media_locator source{LAPIS_LOCATOR_FILE, LAPIS_TEST_MEDIA_PATH};
    lapis_open_result info{};
    CHECK(lapis_session_open(session, &source, lapis_playback_epoch{1}, &info) ==
          LAPIS_OK);
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{0}, nullptr, 0, &count) ==
          LAPIS_E_STALE_EPOCH);
    CHECK(count == 0);
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{1}, nullptr, 0, &count) == LAPIS_OK);
    CHECK(count == 0); // Y4M asset has video only; do not invent subtitles.

    lapis_subtitle_track_info untouched{};
    untouched.stream_index = 999;
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{1}, &untouched, 1, &count) == LAPIS_OK);
    CHECK(count == 0 && untouched.stream_index == 999);

    CHECK(lapis_session_close(session, lapis_playback_epoch{2}) == LAPIS_OK);
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{1}, nullptr, 0, &count) ==
          LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_list_subtitle_tracks(
        session, lapis_playback_epoch{2}, nullptr, 0, &count) ==
          LAPIS_E_INVALID_STATE);
    lapis_session_destroy(session);
    lapis_runtime_destroy(runtime);
    std::cout << "embedded subtitle metadata and C ABI epoch fence passed\n";
}
