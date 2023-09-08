#include <iostream>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <vpx/vpx_image.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <vp9/common/vp9_common.h>
#include <mkvmuxer/mkvwriter.h>

#include <x264.h>

extern "C"
{
#include <libavformat/avformat.h>
    // #include "libswscale/swscale.h"
}

// Modified mkvmuxer::MkvWriter that writes to a memory buffer instead of a file
class MemoryBufferMkvWriter : public mkvmuxer::IMkvWriter
{
public:
    MemoryBufferMkvWriter(std::vector<uint8_t> &buffer) : buffer_(buffer) {}

    virtual int64_t Position() const override
    {
        return position_;
    }

    virtual int32_t Position(int64_t position) override
    {
        if (position < 0 || static_cast<size_t>(position) > buffer_.size())
        {
            return -1;
        }
        position_ = position;
        return 0;
    }

    virtual bool Seekable() const override
    {
        return true;
    }

    virtual int32_t Write(const void *buf, uint32_t len) override
    {
        const size_t new_size = position_ + len;
        if (new_size > buffer_.size())
        {
            buffer_.resize(new_size);
        }
        std::memcpy(buffer_.data() + position_, buf, len);
        position_ += len;
        return 0;
    }

    virtual void ElementStartNotify(uint64_t element_id, int64_t position) override
    {
        // No op
        // Could put some logging in here
    }

private:
    std::vector<uint8_t> &buffer_;
    size_t position_ = 0;
};

// Given a width, height, and time component generate a XOR texture
vpx_image_t *genXorTexture(int width, int height, int time)
{
    if (width <= 0 || height <= 0)
    {
        std::cerr << "Invalid dimensions provided." << std::endl;
        return nullptr;
    }

    vpx_image_t *img = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 1);
    if (!img)
    {
        std::cerr << "Failed to allocate image." << std::endl;
        return nullptr;
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint8_t value = x ^ y ^ (time & 0xFF);
            // Set the Y (luminance) component to the XOR value
            img->planes[VPX_PLANE_Y][y * img->stride[VPX_PLANE_Y] + x] = value;

            // For the I420 format, U and V (chrominance) planes are quarter resolution.
            // Set them to 128 (neutral value) to get a grayscale image.
            if (x % 2 == 0 && y % 2 == 0)
            {
                img->planes[VPX_PLANE_U][y / 2 * img->stride[VPX_PLANE_U] + x / 2] = 128;
                img->planes[VPX_PLANE_V][y / 2 * img->stride[VPX_PLANE_V] + x / 2] = 128;
            }
        }
    }

    return img;
}

static int encode_frame(vpx_codec_ctx_t *codec, vpx_image_t *img, int frame_index, int flags, mkvmuxer::IMkvWriter *writer, mkvmuxer::Segment &segment, const uint64_t &track)
{
    int got_pkts = 0;
    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt = NULL;

    const vpx_codec_err_t res = vpx_codec_encode(codec, img, frame_index, 1, flags, VPX_DL_GOOD_QUALITY);
    if (res != VPX_CODEC_OK)
    {
        std::cerr << "Error during encoding: " << vpx_codec_error(codec);
        const char *detail = vpx_codec_error_detail(codec);
        if (detail)
        {
            std::cerr << " - " << detail;
        }
        std::cerr << std::endl;
        exit(1);
    }

    while ((pkt = vpx_codec_get_cx_data(codec, &iter)) != NULL)
    {
        got_pkts = 1;

        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT)
        {
            // std::cout << "Frame index: " << frame_index << " - Frame size: " << pkt->data.frame.sz << std::endl;
            const int keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            // std::cout << "Frame PTS: " << pkt->data.frame.pts << std::endl;
            // TODO: reference the 30 as a frame rate variable
            int addFrame = segment.AddFrame(static_cast<const uint8_t *>(pkt->data.frame.buf), pkt->data.frame.sz, track, pkt->data.frame.pts * 1e9 / 30, keyframe);
            // std::cout << "Add frame result: " << addFrame << std::endl;
            if (!addFrame)
            {
                std::cerr << "Failed to add frame to webm" << std::endl;
                exit(1);
            }
        }
    }

    return got_pkts;
}




const int FRAME_RATE = 30; // fps
const int SEGMENT_DURATION = 10; // in seconds

void generateXorTexture(x264_picture_t *pic, int width, int height, int time)
{
    std::cout << pic << std::endl;
    if (x264_picture_alloc(pic, X264_CSP_I420, width, height) < 0)
    {
        std::cerr << "Failed to allocate picture" << std::endl;
    }

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            uint8_t value = x ^ y ^ (time & 0xFF);
            pic->img.plane[0][y * width + x] = value; // Y channel (luminance)
            pic->img.plane[1][y * width / 2 + x / 2] = 128; // U channel (128 for gray)
            pic->img.plane[2][y * width / 2 + x / 2] = 128; // V channel (128 for gray)
        }
    }
}

// TODO: instead of using x264 codec use the avformat codec for the frames
// Known issue where the stream will start off fine but will get noiser over time, likely due to the fact that the layout of the frames is different, eventually memory management issues
// All the issues in this function are likely related to this
std::vector<uint8_t> generateHLSSegment(int width, int height, int64_t pts_offset = 0)
{
    std::cout << "Generating HLS segment" << std::endl;

    std::vector<uint8_t> ts_data;

    AVFormatContext *outctx = nullptr;
    avformat_alloc_output_context2(&outctx, nullptr, "mpegts", nullptr);

    AVStream *stream = avformat_new_stream(outctx, nullptr);
    stream->time_base.num = 1;
    stream->time_base.den = FRAME_RATE;
    // stream->duration = 300; // in time_base units

    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->width = width;
    stream->codecpar->height = height;

    avio_open_dyn_buf(&outctx->pb);
    avformat_write_header(outctx, nullptr);

    // Initialize the encoder
    x264_param_t param;
    x264_param_default_preset(&param, "fast", "zerolatency"); // or ultrafast
    // param.i_threads = 1;
    param.i_csp = X264_CSP_I420;
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = FRAME_RATE;
    param.i_fps_den = 1;
    param.b_vfr_input = 0;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    if (x264_param_apply_profile(&param, "high") < 0)
    {
        std::cerr << "Failed to apply profile restrictions" << std::endl;
    }

    x264_t *encoder = x264_encoder_open(&param);
    if (!encoder)
    {
        std::cerr << "Failed to open encoder" << std::endl;
        return {};
    }

    int64_t timestamp = 0;
    int64_t num_frames = FRAME_RATE * SEGMENT_DURATION; // The number of frames in a segment
    std::cout << "Encoding " << num_frames << " frames" << std::endl;
    for (int64_t i = pts_offset; i < pts_offset + num_frames; i++)
    {
        std::cout << "Encoding frame " << i << std::endl;
        x264_picture_t in_pic;
        x264_picture_t out_pic;
        // Sometimes there is an error thrown around here: malloc: Region cookie corrupted for region 0x150000000 (value is 80808080)[0x150007ffc]
        generateXorTexture(&in_pic, width, height, i);
        std::cout << "Made texture" << std::endl;

        in_pic.i_pts = i;
        std::cout << "Frame " << i << " PTS: " << in_pic.i_pts << std::endl;

        x264_nal_t *nals; // Network abstraction layer, essentially these are groups of packets
        int i_nals;

        int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, &in_pic, &out_pic); // Encode the frame

        if (frame_size < 0) // Is this actually an error? Doesn't it just mean the frame was buffered?
        {
            std::cerr << "Failed to encode frame" << std::endl;
        }
        else
        { // Mux the encoded frame into the stream
            for (int j = 0; j < i_nals; j++)
            {
                AVPacket pkt;
                av_init_packet(&pkt);

                // pkt.time_base = (AVRational){1, FRAME_RATE};
                // std::cout << "pkt time base: " << pkt.time_base.num << "/" << pkt.time_base.den << std::endl;

                pkt.data = nals[j].p_payload;
                pkt.size = nals[j].i_payload;
                // pkt.flags |= AV_PKT_FLAG_KEY;

                //  dts <= pts
                //  TODO: something about this is wrong, the image gets noisier over time so pts may be slightly off?
                // Could be be bc of dts discontinuities? see ffprobe output
                pkt.dts = av_rescale_q(in_pic.i_pts, (AVRational){1, FRAME_RATE}, stream->time_base) - i_nals + j;
                pkt.pts = av_rescale_q(in_pic.i_pts, (AVRational){1, FRAME_RATE}, stream->time_base);
                std::cout << "pkt dts: " << pkt.dts << std::endl;
                std::cout << "pkt pts: " << pkt.pts << std::endl;

                // Sometimes there's segfults here?
                if (av_interleaved_write_frame(outctx, &pkt) != 0)
                {
                    std::cerr << "Failed to write frame" << std::endl;
                }
                else
                {
                    std::cout << "Wrote frame " << i << std::endl;
                }
            }
        }

        // x264_picture_clean(&in_pic); // this line was breaking things?
        // std::cout << "Encoded frame " << i << std::endl;
    }

    // Flush the encoder
    std::cout << "Flushing encoder" << std::endl;
    while (x264_encoder_delayed_frames(encoder))
    {
        std::cout << "Entered flushing loop" << std::endl;
        x264_nal_t *nals;
        int i_nals;
        x264_picture_t out_pic;

        int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, NULL, &out_pic);
        if (frame_size < 0)
        {
            std::cerr << "Encoder failed while flushing" << std::endl;
        }
        else
        { // Mux the remaining encoded frames into the stream
            for (int j = 0; j < i_nals; j++)
            {
                // std::cout << "in write nal loop" << std::endl;
                AVPacket pkt;
                av_init_packet(&pkt);
                pkt.data = nals[j].p_payload;
                pkt.size = nals[j].i_payload;
                // does the pts and dts need to be set here? we almost never need to flush the encoder so we typically never get here
                av_write_frame(outctx, &pkt);
            }
        }
    }
    std::cout << "Flushing complete" << std::endl;

    // Still some double free error here?
    x264_encoder_close(encoder);
    // x264_picture_clean(&in_pic); // The example has this line?
    std::cout << "Encoder closed" << std::endl;

    av_write_trailer(outctx);

    uint8_t *buffer;
    int size = avio_close_dyn_buf(outctx->pb, &buffer);
    ts_data.insert(ts_data.end(), buffer, buffer + size);
    av_free(buffer);

    // Cleanup
    avformat_free_context(outctx);
    std::cout << "bangarang" << std::endl;
    return ts_data;
}

void saveSegmentToFile(const std::vector<uint8_t> &segment, const std::string &filename)
{
    std::ofstream outfile(filename, std::ios::out | std::ios::binary);
    if (!outfile)
    {
        std::cerr << "Could not open file for writing: " << filename << std::endl;
        return;
    }

    outfile.write((char *)&segment[0], segment.size());
    outfile.close();
}

std::vector<std::string> playlist_segments = {"segment_0.ts"}; // Ensure there is one segment to play at first
int media_sequence = 0;

int main()
{
    httplib::Server svr;

    // std::cout << "Current working directory: " << std::filesystem::current_path() << std::endl;
    // for(auto & p : std::filesystem::directory_iterator("../public"))
    // std::cout << p << std::endl;

    auto ret = svr.set_mount_point("/", "../public");
    if (!ret)
    {
        throw std::runtime_error("Failed to set mount point");
    }

    // Load file into memory
    std::string filePath = "../public/big-buck-bunny_trailer.webm";
    std::ifstream file(filePath, std::ifstream::binary | std::ifstream::ate);
    if (!file)
    {
        std::cout << "Error opening file: " << filePath << std::endl;
        return 0;
    }
    std::streamsize fileSize = file.tellg(); // Get the file size
    file.seekg(0, std::ios::beg);            // Reset the file pointer to the beginning

    // A semi-complete implementation of range requests
    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Range
    // https://datatracker.ietf.org/doc/html/rfc7233

    // Max size of each range request to fulfill in bytes
    const size_t MAX_RESPONSE_SIZE = 10 * 1024;

    /*
    tldr: basic video live stream where new video is continuously appended using MSE, many issues and does not work in practice
    Client code is `/old-stream.html`

    A basic continuous streaming set up where the server responds with unknown length content ranges
    would work since <video/> should play a continuous stream as long as data keeps coming, but isn't great
    since a lot of additional complexity would have to be handled, like network changes, skipped frames, smooth playback, video player exceptions, etc.
    An actual video streaming solution is more optimal, like HLS
    */
    
    /*
    svr.Get("/stream", [](const httplib::Request &req, httplib::Response &res)
            {
                const int width = 640;
                const int height = 480;
                const int num_frames = 100; // this is a stream, we're going to append segments of 100 fr

                vpx_codec_ctx_t codec;
                vpx_codec_enc_cfg_t cfg;
                vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0);
                cfg.g_w = width;
                cfg.g_h = height;
                cfg.g_timebase.num = 1;
                cfg.g_timebase.den = 30; // 30 fps
                // cfg.g_error_resilient = VPX_ERROR_RESILIENT_PARTITIONS;

                if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK)
                {
                    std::cerr << "Failed to initialize encoder: " << vpx_codec_error(&codec) << std::endl;
                    return;
                }

                std::vector<uint8_t> webmData;
                MemoryBufferMkvWriter memWriter(webmData);
                // mkvmuxer::MkvWriter memWriter;
                // memWriter.Open("test.webm");

                mkvmuxer::Segment segment;
                mkvmuxer::SegmentInfo *const info = segment.GetSegmentInfo();
                info->set_writing_app("XorTextureGenerator");
                info->set_timecode_scale(1e9 * (static_cast<double>(cfg.g_timebase.num) / cfg.g_timebase.den));
                // segment.set_duration(num_frames); // This duration is not actually needed and will be calculated automatically, useful for streaming
                // std::cout << "Duration: " << segment.duration() << std::endl;

                const uint64_t track = segment.AddVideoTrack(width, height, 0);
                if (!track)
                {
                    std::cerr << "Failed to add video track" << std::endl;
                    return;
                }

                mkvmuxer::VideoTrack *const video = static_cast<mkvmuxer::VideoTrack *>(
                    segment.GetTrackByNumber(track));
                if (!video)
                {
                    printf("\n Could not get video track.\n");
                    return;
                }
                video->set_default_duration(uint64_t(1e9 * (static_cast<double>(cfg.g_timebase.num) / cfg.g_timebase.den))); // duration of each frame in nanoseconds
                video->set_codec_id("V_VP9");
                video->set_display_width(width);
                video->set_display_height(height);
                video->set_pixel_width(width);
                video->set_pixel_height(height);

                if (!segment.Init(&memWriter))
                {
                    std::cerr << "Failed to initialize muxer segment" << std::endl;
                    return;
                }

                int frame_count = 0;
                // std::cout << "Starting encoding" << std::endl;
                while (frame_count < num_frames)
                {
                    vpx_image_t *img = genXorTexture(width, height, frame_count);
                    encode_frame(&codec, img, frame_count++, 0, &memWriter, segment, track);
                    vpx_img_free(img);
                }
                // std::cout << "Encoding complete" << std::endl;

                // Signal to encoder that we are done
                // std::cout << "Starting flushing" << std::endl;
                while (encode_frame(&codec, nullptr, -1, 0, &memWriter, segment, track))
                {
                    // Flush any remaining frames
                }
                // std::cout << "Flushing complete" << std::endl;

                if (!segment.Finalize())
                {
                    std::cerr << "Error finalizing segment." << std::endl;
                    exit(1);
                }

                vpx_codec_destroy(&codec);

                std::string range = req.get_header_value("Range");
                size_t start = 0;
                size_t end = start + MAX_RESPONSE_SIZE - 1; // Respect the MAX_RESPONSE_SIZE constraint

                if (!range.empty())
                {
                    size_t equalsPos = range.find('=');
                    size_t dashPos = range.find('-');
                    start = std::stoull(range.substr(equalsPos + 1, dashPos - equalsPos - 1));
                }

                // Ensure the 'end' doesn't exceed the webmData size
                end = std::min(end, webmData.size() - 1);
                std::cout << "Start: " << start << " - End: " << end << std::endl;

                // Content range with unknown total size
                std::string contentRange = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/*";

                std::vector<uint8_t> responseData(webmData.begin() + start, webmData.begin() + end + 1);

                res.status = 206; // Partial Content
                res.set_header("Content-Range", contentRange.c_str());
                res.set_content(reinterpret_cast<const char *>(responseData.data()), responseData.size(), "video/webm");
            });
    */

    svr.Get("/playlist.m3u8", [](const httplib::Request &req, httplib::Response &res)
            {
                std::cout << "Playlist requested. " << playlist_segments.size() <<  " segments exist." <<std::endl;
                std::string content = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n";
                
                if (playlist_segments.size() > 0) {
                    content += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(media_sequence) + "\n";
                    
                    for (const auto& segment_name : playlist_segments) {
                        content += "#EXTINF:10.0,\n" + segment_name + "\n";
                    }
                }

                res.set_content(content, "application/vnd.apple.mpegurl"); });

    svr.Get(R"(/segment_(\d+)\.ts)", [](const httplib::Request &req, httplib::Response &res)
            {
                std::cout << "Segment requested" << std::endl;
                int segment_index = std::stoi(req.matches[1]);
                std::cout << "Segment index: " << segment_index << std::endl;
                int64_t offset = segment_index * FRAME_RATE * SEGMENT_DURATION; // The number of pts to offset the segment

                // pass segment index * num_frames_per_segment
                std::vector<uint8_t> segment = generateHLSSegment(1280, 720, offset);
                std::string segment_name = "segment_" + std::to_string(segment_index) + ".ts";
                saveSegmentToFile(segment, segment_name);

                // every time this segment is requested, add a new one to the playlist
                playlist_segments.push_back("segment_" + std::to_string(segment_index+1) + ".ts");

                res.set_content(std::string(segment.begin(), segment.end()), "video/MP2T"); });

    svr.Get("/webm", [](const httplib::Request &req, httplib::Response &res)
            {
                const int width = 640;
                const int height = 480;
                const int num_frames = 300; // example

                vpx_codec_ctx_t codec;
                vpx_codec_enc_cfg_t cfg;
                vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0);
                cfg.g_w = width;
                cfg.g_h = height;
                cfg.g_timebase.num = 1;
                cfg.g_timebase.den = 30; // 30 fps
                //cfg.g_error_resilient = VPX_ERROR_RESILIENT_PARTITIONS;

                if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) {
                    std::cerr << "Failed to initialize encoder: " << vpx_codec_error(&codec) << std::endl;
                    return;
                }
                
                std::vector<uint8_t> webmData;
                MemoryBufferMkvWriter memWriter(webmData);
                //mkvmuxer::MkvWriter memWriter; // not actually an in memory writer, variable is just named that for consistency
                //memWriter.Open("test.webm");

                mkvmuxer::Segment segment;
                mkvmuxer::SegmentInfo *const info = segment.GetSegmentInfo();
                info->set_writing_app("XorTextureGenerator");
                info->set_timecode_scale(1e9 * (static_cast<double>(cfg.g_timebase.num) / cfg.g_timebase.den));
                //segment.set_duration(num_frames); // This duration is not actually needed and will be calculated automatically
                //std::cout << "Duration: " << segment.duration() << std::endl;

                // TODO: start adding functionality here

                const uint64_t track = segment.AddVideoTrack(width, height, 0);
                if (!track) {
                    std::cerr << "Failed to add video track" << std::endl;
                    return;
                }

                mkvmuxer::VideoTrack* const video = static_cast<mkvmuxer::VideoTrack*>(
                    segment.GetTrackByNumber(track));
                if (!video) {
                    printf("\n Could not get video track.\n");
                    return;
                }
                video->set_default_duration(uint64_t (1e9 * (static_cast<double>(cfg.g_timebase.num) / cfg.g_timebase.den))); // duration of each frame in nanoseconds
                video->set_codec_id("V_VP9");
                video->set_display_width(width);
                video->set_display_height(height);
                video->set_pixel_width(width);
                video->set_pixel_height(height);

                if (!segment.Init(&memWriter)) {
                    std::cerr << "Failed to initialize muxer segment" << std::endl;
                    return;
                }
                
                int frame_count = 0;
                //std::cout << "Starting encoding" << std::endl;
                while(frame_count < num_frames){
                    // Add keyframe interval?
                    //std::cout << "Encoding frame " << frame_count << std::endl;
                    vpx_image_t *img = genXorTexture(width, height, frame_count);
                    encode_frame(&codec, img, frame_count++, 0, &memWriter, segment, track);
                    vpx_img_free(img);
                }
                //std::cout << "Encoding complete" << std::endl;

                // Signal to encoder that we are done
                //std::cout << "Starting flushing" << std::endl;
                while (encode_frame(&codec, nullptr, -1, 0, &memWriter, segment, track)) {
                    // Flush any remaining frames
                }
                //std::cout << "Flushing complete" << std::endl;
                
                if (!segment.Finalize()) {
                    std::cerr << "Error finalizing segment." << std::endl;
                    exit(1);
                }

                //memWriter.Close();
                //std::cout << "Video written" << std::endl;

                vpx_codec_destroy(&codec);



                std::string range = req.get_header_value("Range");
                size_t start = 0;
                size_t end = webmData.size() - 1;

                if (!range.empty()) {
                    size_t equalsPos = range.find('=');
                    size_t dashPos = range.find('-');
                    start = std::stoull(range.substr(equalsPos + 1, dashPos - equalsPos - 1));
                    if (dashPos != std::string::npos && dashPos != range.size() - 1) {
                        end = std::stoull(range.substr(dashPos + 1));
                    }
                }

                if (start > end || end >= webmData.size())
                {
                    res.status = 416; // Range Not Satisfiable
                    return;
                }

                std::string contentRange = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(webmData.size());

                std::vector<uint8_t> responseData(webmData.begin() + start, webmData.begin() + end + 1);

                res.status = 206; // Partial Content
                res.set_header("Content-Range", contentRange.c_str());
                res.set_content(reinterpret_cast<const char *>(responseData.data()), responseData.size(), "video/webm"); });

    /*
    svr.Get("/video/big-buck-bunny_trailer.webm", [&](const httplib::Request &req, httplib::Response &res)
            {
                std::string range = req.get_header_value("Range");
                std::string contentRange = "bytes 0-" + std::to_string(fileSize - 1) + "/" + std::to_string(fileSize); // Default range is the entire file

                if (!range.empty())
                { // If range header present
                    std::size_t equalsPos = range.find('=');
                    std::size_t dashPos = range.find('-');
                    std::streamsize start = std::stoll(range.substr(equalsPos + 1, dashPos - equalsPos - 1)); // Start byte of range
                    std::streamsize end = (dashPos == std::string::npos || dashPos == range.size() - 1)       // End byte of range
                                              ? fileSize - 1
                                              : std::stoll(range.substr(dashPos + 1));

                    // Check if the range is not satisfiable
                    // TODO: Check if start < 0 ?
                    if (start > end || end >= fileSize)
                    {
                        res.status = 416; // 416 Range Not Satisfiable
                        return;
                    }

                    fileSize = end - start + 1;
                    file.seekg(start);
                    // Define the requested & validated range
                    contentRange = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize);
                }

                // Read the file into a buffer
                char *buffer = new char[fileSize];
                file.read(buffer, fileSize);
                std::string content(buffer, fileSize);
                delete[] buffer;

                // Deliver the desired range
                res.status = 206; // 206 Partial Content
                res.set_content(content, "video/webm");
                res.set_header("Content-Range", contentRange.c_str()); });
    */

    svr.Get("/ping", [](const httplib::Request &, httplib::Response &res)
            { res.set_content("pong", "text/plain"); });

    // Catch-all 404 route
    // svr.Get(".*", [](const httplib::Request &, httplib::Response &res) {
    //     res.status = 404;
    //     res.set_content("404 Not Found", "text/plain");
    // });

    svr.listen("0.0.0.0", 8080);

    return 0;
}