﻿
#include "ffmpegWrapper.h"
#include <thread>         // std::this_thread::sleep_for
#include <chrono>         // std::chrono::seconds
#include "error.h"

#include <libavutil/ffversion.h>
#include <config.h>

#pragma warning (disable: 4996)
#pragma warning (disable: 4819)

const enum AVPixelFormat TARGET_PIX_FMT = AV_PIX_FMT_YUV420P;
const enum AVHWDeviceType HW_DEVICE_TYPE = AV_HWDEVICE_TYPE_DXVA2; // ;  AV_HWDEVICE_TYPE_D3D11VA
const int AV_RAW_HEADER_SIZE = 8;
const int DISCARD_FRAME_FREQUENCY = 2;

#define RESOLUTION_NUM (5)
#define SAMPLERATE_NUM (7)
const int vResolution_[RESOLUTION_NUM][2] = {
        {256,  144},
        {640,  360},    // 建议模式：16分屏
        {800,  600},    //            9分屏
        {1280, 720},   //            4分屏
        {1920, 1080}   //            1分屏
};
const int vSampleRate_[SAMPLERATE_NUM] = {
        8000,
        12000,
        16000,
        32000,
        44100,
        48000,
        96000
};

static int getSampleRateInddex(int s) {
    for (int i = 0; i < SAMPLERATE_NUM; i++) {
        if (vSampleRate_[i] == s) {
            return i;
        }
    }
    return -1;
}

static int getResolutionIndex(int w, int h) {

    for (int i = 0; i < RESOLUTION_NUM; i++) {
        if ((vResolution_[i][0] == w) && (vResolution_[i][1] == h)) {
            return i;
        }
    }
    return -1;
}

static int judgeVideoResolution(uint16_t width, uint16_t height) {
    int ret = -1;
    for (const auto &vr: vResolution_) {
        if (vr[0] != width) {
            continue;
        }
        return vr[1] == height ? 0 : -1;
    }
    return ret;
}

AVPixelFormat FfmpegWrapper::hw_pix_fmt_ = AV_PIX_FMT_DXVA2_VLD;

FfmpegWrapper::FfmpegWrapper() : fmt_ctx_(nullptr), format_options_(nullptr), sws_init_(false), sws_ctx_(nullptr),
                                 video_dec_ctx_(nullptr), audio_dec_ctx_(nullptr), hw_device_ctx_(nullptr),
                                 sw_frame_(nullptr), frameYUV420_(nullptr), stop_request_(0), video_dst_data_(nullptr),
                                 audio_dst_data_(nullptr), current_pts_audio_in_ms_(0), current_pts_video_in_ms_(0),
                                 output_width_(-1), output_height_(-1), audio_stream_(nullptr), video_stream_(nullptr),
                                 useGPU_(0), user_data_(nullptr), user_handle_(0), discard_frame_index_(0),
                                 discard_frame_enabled_(1) {
#ifdef _DEBUG
    showBanner();
    av_log_set_level(AV_LOG_INFO);
#else
    av_log_set_level(AV_LOG_FATAL);
#endif
}

FfmpegWrapper::~FfmpegWrapper() {
    if (!stop_request_) {
        stopPlay();
    }
}

int FfmpegWrapper::showBanner() {
    av_log(NULL, AV_LOG_WARNING, "\nversion " FFMPEG_VERSION);
    av_log(NULL, AV_LOG_WARNING, " built with %s\n", CC_IDENT);
    av_log(NULL, AV_LOG_WARNING, "configuration: " FFMPEG_CONFIGURATION "\n\n");
    return 0;
}

int FfmpegWrapper::setSendDataCallback(void *user, uintptr_t handle, const FF_SEND_DATA_CALLBACK &pfn) {
    user_data_ = user;
    user_handle_ = handle;
    ff_send_data_callback_ = pfn;
    return 0;
}

int FfmpegWrapper::setExceptionCallback(void *user, uintptr_t handle, const FF_EXCEPTION_CALLBACK &pfn) {
    user_data_ = user;
    user_handle_ = handle;
    ff_exception_callback_ = pfn;
    return 0;
}

int FfmpegWrapper::startPlay(const char *inputUrl, int width, int height,
                             int useGPU /*= 1*/, int useTCP /*= 1*/, int retryTimes/* = 3*/) {
    LOG_INFO << "[" << user_handle_ << "]startPlay url[" << inputUrl << "], GPU:" << useGPU;
    this->output_width_ = width;
    this->output_height_ = height;
    this->useGPU_ = useGPU;
    this->inputUrl_ = inputUrl;

    main_read_thread_handle_ = std::move(std::thread([this, useTCP, useGPU, retryTimes]() mutable {
        int ret = 0;
        int video_stream_index;
        int audio_stream_index;
        AVPacket *pkt = nullptr;
        av_dict_set(&format_options_, "rtsp_transport", useTCP ? "tcp" : "udp", 0); // 设置RTSP传输模式
        do {
            if ((ret = open_input_url(inputUrl_.c_str(), retryTimes)) != 0) {
                break;
            }

            /* retrieve stream information */
            if ((ret = avformat_find_stream_info(fmt_ctx_, NULL)) < 0) {
                LOG_ERROR << "Could not find stream information";
                break;
            }

            if (open_codec_context(&video_stream_index, &video_dec_ctx_, fmt_ctx_, AVMEDIA_TYPE_VIDEO) >= 0) {
                video_stream_ = fmt_ctx_->streams[video_stream_index];
                if (0 != judgeVideoResolution(output_width_, output_height_)) {
                    output_width_ = video_dec_ctx_->width;
                    output_height_ = video_dec_ctx_->height;
                }
            }

            if (open_codec_context(&audio_stream_index, &audio_dec_ctx_, fmt_ctx_, AVMEDIA_TYPE_AUDIO) >= 0) {
                audio_stream_ = fmt_ctx_->streams[audio_stream_index];
            }

            /* dump input information to stderr */
            av_dump_format(fmt_ctx_, 0, inputUrl_.c_str(), 0);

            if (!audio_stream_ && !video_stream_) {
                LOG_ERROR << "Could not find audio or video stream in the input, aborting";
                ret = -1;
                break;
            }

            sw_frame_ = av_frame_alloc();
            if (!sw_frame_) {
                LOG_ERROR << "Could not allocate frame";
                ret = AVERROR(ENOMEM);
                break;
            }

            pkt = av_packet_alloc();
            if (!pkt) {
                LOG_ERROR << "Could not allocate packet";
                ret = AVERROR(ENOMEM);
                break;
            }
        } while (0);

        if (ret != 0 && ff_exception_callback_ && user_data_) {
            ff_exception_callback_(user_data_, user_handle_, ret, (uint8_t *) "open url failed.");
            return;
        }

        /* read frames from the input */
        while (!stop_request_) {
            ret = av_read_frame(fmt_ctx_, pkt);
            if (ret < 0 || !pkt) {
                if (fmt_ctx_->pb && fmt_ctx_->pb->error)
                    break;

                std::this_thread::sleep_for(chrono::milliseconds(10));
                continue;
            }
            preTime_ = time(nullptr);

            // check if the packet belongs to a stream we are interested in, otherwise
            // skip it
            if (pkt->stream_index == video_stream_index)
                video_packet_queue_.put(pkt);
            else if (pkt->stream_index == audio_stream_index) {
                audio_packet_queue_.put(pkt);
            }
            av_packet_unref(pkt);
        }

        /* flush the decoders */
        if (video_dec_ctx_)
            video_packet_queue_.put(pkt);
        if (audio_dec_ctx_)
            audio_packet_queue_.put(pkt);

        if (!stop_request_ && ff_exception_callback_ && user_data_) {
            char buf[128] = {0};
            av_make_error_string(buf, 128, ret);
            ff_exception_callback_(user_data_, user_handle_, ret, (uint8_t *) buf);
        }
        av_packet_free(&pkt);
    }));
    audio_decode_thread_handle_ = std::thread(&FfmpegWrapper::audio_decode_thread, this);
    video_decode_thread_handle_ = std::thread(&FfmpegWrapper::video_decode_thread, this);

    return 0;
}

int FfmpegWrapper::stopPlay() {
    LOG_INFO << "[" << user_handle_ << "]stopPlay";
    stop_request_ = 1;
    video_packet_queue_.stop();
    audio_packet_queue_.stop();
    if (audio_decode_thread_handle_.joinable()) {
        audio_decode_thread_handle_.join();
    }
    if (video_decode_thread_handle_.joinable()) {
        video_decode_thread_handle_.join();
    }
    if (main_read_thread_handle_.joinable()) {
        main_read_thread_handle_.join();
    }

    av_dict_free(&format_options_);
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    avcodec_free_context(&video_dec_ctx_);
    avcodec_free_context(&audio_dec_ctx_);
    avformat_close_input(&fmt_ctx_);
    av_buffer_unref(&hw_device_ctx_);

    av_frame_free(&sw_frame_);
    av_frame_free(&frameYUV420_);
    av_freep(&video_dst_data_);
    av_freep(&audio_dst_data_);
    return 0;
}

int FfmpegWrapper::changeVideoResolution(int width, int height) {
    if (width == 0 || height == 0) {
        output_width_ = video_dec_ctx_->width;
        output_height_ = video_dec_ctx_->height;
    } else {
        output_width_ = width;
        output_height_ = height;
    }

    sws_init_ = false;

    return 0;
}

int FfmpegWrapper::openDiscardFrames(int enabled) {
    discard_frame_enabled_ = enabled;
    return 0;
}

int FfmpegWrapper::output_video_frame(AVFrame *frame) {
    int ret = 0;
    AVFrame *tmp_frame = nullptr;
    AVFrame *tmp_frame2 = nullptr;

    // 抽帧
    if (discard_frame_enabled_ && (++discard_frame_index_ % DISCARD_FRAME_FREQUENCY == 0)) {
        return 0;
    }

    if (frame->format == hw_pix_fmt_) {
        /* retrieve data from GPU to CPU */
        if ((ret = av_hwframe_transfer_data(sw_frame_, frame, 0)) < 0) {
            LOG_ERROR << "Error transferring the data to system memory";
            // TODO: 错误处理
            return 0;
        }
        tmp_frame = sw_frame_;
    } else {
        tmp_frame = frame;
    }

    // 分辨率改变之后，需要重新初始化sws
    if (tmp_frame->width != output_width_ || tmp_frame->height != output_height_ ||
        tmp_frame->format != TARGET_PIX_FMT) {
        if (!sws_init_) {
            if (sws_ctx_) {
                sws_freeContext(sws_ctx_);
                sws_ctx_ = nullptr;
            }
            av_frame_free(&frameYUV420_);
            av_freep(&video_dst_data_);
        }
        if (!sws_ctx_) {
            // 如果明确是要缩小并显示，建议使用SWS_POINT算法
            // 在不明确是放大还是缩小时，直接使用 SWS_FAST_BILINEAR 算法即可。
            sws_ctx_ = sws_getContext(tmp_frame->width, tmp_frame->height,
                                      (AVPixelFormat) tmp_frame->format,
                                      output_width_, output_height_,
                                      TARGET_PIX_FMT,
                                      SWS_POINT, NULL, NULL, NULL);
        }

        if (!frameYUV420_) {
            frameYUV420_ = av_frame_alloc();
            if (!frameYUV420_) {
                LOG_ERROR << "Could not allocate frame";
                ret = AVERROR(ENOMEM);
                return ret;
            }

            frameYUV420_->format = TARGET_PIX_FMT;
            frameYUV420_->width = output_width_;
            frameYUV420_->height = output_height_;

            ret = av_frame_get_buffer(frameYUV420_, 0);
            if (ret < 0) {
                LOG_ERROR << "Could not av_frame_get_buffer";
                return ret;
            }
            sws_init_ = true;
            // int numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height);
        }

        ret = sws_scale(sws_ctx_, (const uint8_t *const *) tmp_frame->data, tmp_frame->linesize,
                        0, tmp_frame->height, frameYUV420_->data, frameYUV420_->linesize);
        if (ret != frameYUV420_->height) {
            LOG_ERROR << "Could not sws_scale frame";
            return ret;
        }
        tmp_frame2 = frameYUV420_;
    } else {
        tmp_frame2 = tmp_frame;
    }

    int size = av_image_get_buffer_size(TARGET_PIX_FMT, output_width_, output_height_, 1);
    if (!video_dst_data_) {
        video_dst_data_ = (uint8_t *) av_malloc(size + AV_RAW_HEADER_SIZE);
        if (!video_dst_data_) {
            LOG_ERROR << "Can not alloc buffer";
            ret = AVERROR(ENOMEM);
            return ret;
        }
    }

    if (frame->pts == AV_NOPTS_VALUE) {
        frame->pts = 0;
    } else if (frame->pts < 0) {
        return 0;
    }
    current_pts_video_in_ms_ = av_q2d(video_stream_->time_base) * frame->pts * 1000;

    video_dst_data_[0] = FF_MEDIA_TYPE_VIDEO; //video flag
    video_dst_data_[1] = getResolutionIndex(output_width_, output_height_);
    video_dst_data_[2] = 0x01;
    video_dst_data_[3] = 0x01;
    video_dst_data_[4] = (uint8_t) (current_pts_video_in_ms_ >> 24);
    video_dst_data_[5] = (uint8_t) (current_pts_video_in_ms_ >> 16);
    video_dst_data_[6] = (uint8_t) (current_pts_video_in_ms_ >> 8);
    video_dst_data_[7] = (uint8_t) (current_pts_video_in_ms_);

    ret = av_image_copy_to_buffer(video_dst_data_ + AV_RAW_HEADER_SIZE, size,
                                  (const uint8_t *const *) tmp_frame2->data,
                                  (const int *) tmp_frame2->linesize, TARGET_PIX_FMT,
                                  output_width_, output_height_, 1);
    if (ret < 0) {
        LOG_ERROR << "Can not copy image to buffer";
        return ret;
    }

    /* write to rawvideo file */
    if (0) {
        FILE *fp = nullptr;
        fopen_s(&fp, "420P.yuv", "ab");
        fwrite(video_dst_data_ + AV_RAW_HEADER_SIZE, AV_RAW_HEADER_SIZE, size, fp);
        fclose(fp);
    }

    if (ff_send_data_callback_ && user_data_) {
        if ((ret = ff_send_data_callback_(user_data_, user_handle_,
                                          video_dst_data_, size + AV_RAW_HEADER_SIZE)) != 0) {
//             if (_ff_exception_callback) {
//                 _ff_exception_callback(_user_data, _user_handle, ret, (uint8_t*)GetErrorInfo(ret));
//             }
        }
    }

    return 0;
}

int FfmpegWrapper::output_audio_frame(AVFrame *frame) {
    int ret = 0;
    size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(AVSampleFormat(frame->format));

    /* Write the raw audio data samples of the first plane. This works
     * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
     * most audio decoders output planar audio, which uses a separate
     * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
     * In other words, this code will write only the first audio channel
     * in these cases.
     * You should use libswresample or libavfilter to convert the frame
     * to packed data. */
    /* write to rawvideo file */
    if (0) {
        FILE *fp = nullptr;
        fopen_s(&fp, "auduio.pcm", "ab");
        if (fp) {
            // 获取一个采样点字节数，比如16位采样点值为2字节
            int data_size = av_get_bytes_per_sample(audio_dec_ctx_->sample_fmt);

            // frame->nb_samples为这个frame中一个声道的采样点的个数
            for (int i = 0; i < frame->nb_samples; i++)
                for (int ch = 0; ch < audio_dec_ctx_->channels; ch++)
                    fwrite(frame->extended_data[ch] + data_size * i, 1, data_size, fp);

            fclose(fp);
        }
    }
    if (!audio_dst_data_) {
        audio_dst_data_ = (uint8_t *) av_malloc(audio_dec_ctx_->channels * unpadded_linesize + AV_RAW_HEADER_SIZE);
    }

    if (!audio_dst_data_) {
        LOG_ERROR << "Can not alloc buffer";
        ret = AVERROR(ENOMEM);
        return ret;
    }

    // 等待有真正的视频帧之后再发送音频
    //while (video_stream_ && current_pts_video_in_ms_ <= 0) {
    //    this_thread::sleep_for(chrono::milliseconds(10));
    //}

    current_pts_audio_in_ms_ = av_q2d(audio_stream_->time_base) * frame->pts * 1000;

    audio_dst_data_[0] = FF_MEDIA_TYPE_AUDIO; // audio flag
    audio_dst_data_[1] = av_get_bytes_per_sample(audio_dec_ctx_->sample_fmt);
    audio_dst_data_[2] = audio_dec_ctx_->channels;
    audio_dst_data_[3] = getSampleRateInddex(audio_dec_ctx_->sample_rate);
    audio_dst_data_[4] = (uint8_t) (current_pts_audio_in_ms_ >> 24);
    audio_dst_data_[5] = (uint8_t) (current_pts_audio_in_ms_ >> 16);
    audio_dst_data_[6] = (uint8_t) (current_pts_audio_in_ms_ >> 8);
    audio_dst_data_[7] = (uint8_t) (current_pts_audio_in_ms_);

    int is_planar = av_sample_fmt_is_planar(audio_dec_ctx_->sample_fmt);
    if (is_planar) {
        // 获取一个采样点字节数，比如16位采样点值为2字节
        int data_size = av_get_bytes_per_sample(audio_dec_ctx_->sample_fmt);

        // frame->nb_samples为这个frame中一个声道的采样点的个数
        int index = 0;
        for (int i = 0; i < frame->nb_samples; i++) {
            for (int ch = 0; ch < audio_dec_ctx_->channels; ch++) {
                memcpy_s(audio_dst_data_ + AV_RAW_HEADER_SIZE + index, audio_dec_ctx_->channels * unpadded_linesize,
                         frame->extended_data[ch] + data_size * i, data_size);
                index += data_size;
            }
        }
    } else {// packet
        memcpy_s(audio_dst_data_ + AV_RAW_HEADER_SIZE, unpadded_linesize, frame->extended_data[0], unpadded_linesize);
    }

    if (ff_send_data_callback_ && user_data_) {
        ff_send_data_callback_(user_data_, user_handle_,
                               audio_dst_data_, audio_dec_ctx_->channels * unpadded_linesize + AV_RAW_HEADER_SIZE);
    }

    return 0;
}

int FfmpegWrapper::decode_packet(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame) {
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        char buf[128] = { 0 };
        av_make_error_string(buf, 128, ret);
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            return ret;
        }

        // write the frame data to output file
        if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
            ret = output_video_frame(frame);
        else
            ret = output_audio_frame(frame);

        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int FfmpegWrapper::open_codec_context(int *stream_idx,
                                      AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type) {
    int ret = 0;
    int stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG_ERROR << "Could not find " << av_get_media_type_string(type)
                  << " stream in input " << inputUrl_;
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            LOG_ERROR << "Failed to find " << av_get_media_type_string(type) << " codec";
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            LOG_ERROR << "Failed to allocate the " << av_get_media_type_string(type) << " codec context";
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            LOG_ERROR << "Failed to copy " << av_get_media_type_string(type)
                      << " codec parameters to decoder context";
            return ret;
        }

        if (useGPU_ && type == AVMEDIA_TYPE_VIDEO && (*dec_ctx)->codec_id == AV_CODEC_ID_HEVC) {
            if (hw_get_config(dec) < 0)
                return -1;
            (*dec_ctx)->get_format = hw_get_format;

            if (hw_decoder_init(*dec_ctx, HW_DEVICE_TYPE) < 0)
                return -1;
        }

        /* Init the decoders */
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            LOG_ERROR << "Failed to open " << av_get_media_type_string(type) << " codec";
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

int FfmpegWrapper::open_input_url(const char *inputUrl, int retryTimes) {
    int ret = 0;
    do {
        if (!(fmt_ctx_ = avformat_alloc_context())) {
            LOG_ERROR << "avformat_alloc_context error";
            ret = -1;
            break;
        }
        fmt_ctx_->interrupt_callback.callback = input_interrupt_cb;
        fmt_ctx_->interrupt_callback.opaque = &preTime_;
        preTime_ = time(nullptr);
        if ((ret = avformat_open_input(&fmt_ctx_, inputUrl, NULL, &format_options_)) == 0) {
            ret = 0;  // success
            break;
        }

        if (retryTimes-- <= 0) {
            char buf[128] = {0};
            av_make_error_string(buf, 128, ret);
            LOG_ERROR << "Could not open source file " << inputUrl << "(" << ret << ")" << buf;
            ret = FFOpenUrlFailed;
            break;
        }
        av_usleep(10000);
    } while (!stop_request_);

    return ret;
}

int FfmpegWrapper::hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type) {
    int ret = 0;

    if ((ret = av_hwdevice_ctx_create(&hw_device_ctx_, type,
                                      NULL, NULL, 0)) < 0) {
        LOG_ERROR << "Failed to create specified HW device.";
        return ret;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    return ret;
}

int FfmpegWrapper::hw_get_config(const AVCodec *decoder) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            LOG_ERROR << "Decoder " << decoder->name
                      << " does not support device type "
                      << av_hwdevice_get_type_name(HW_DEVICE_TYPE);
            return -1;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == HW_DEVICE_TYPE) {
            hw_pix_fmt_ = config->pix_fmt;
            break;
        }
    }
    return 0;
}

enum AVPixelFormat FfmpegWrapper::hw_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt_)
            return *p;
    }

    LOG_ERROR << "Failed to get HW surface format.";
    return AV_PIX_FMT_NONE;
}

void FfmpegWrapper::audio_decode_thread() {
    int ret = 0;
    try {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        do {
            if (stop_request_)
                break;
            if ((ret = audio_packet_queue_.get(pkt)) < 0)
                break;

            ret = decode_packet(audio_dec_ctx_, pkt, frame);
            av_packet_unref(pkt);
            this_thread::sleep_for(chrono::milliseconds(1));
        } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
        av_packet_free(&pkt);
        av_frame_free(&frame);

        // stop_request_ 为1时不需要回调
        if (!stop_request_ && ff_exception_callback_ && user_data_) {
            ff_exception_callback_(user_data_, user_handle_, ret, (uint8_t *) "audio_decode_thread exited.");
        }
    }
    catch (const std::exception &e) {
        LOG_ERROR << "audio thread exception(" << e.what() << ")";
    }
}

void FfmpegWrapper::video_decode_thread() {
    int ret = 0;
    try {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        std::chrono::steady_clock::time_point tp = std::chrono::steady_clock::now();
        do {
            if (stop_request_)
                break;
            if ((ret = video_packet_queue_.get(pkt)) < 0)
                break;

            ret = decode_packet(video_dec_ctx_, pkt, frame);
            av_packet_unref(pkt);
            int duration = 1000000 / av_q2d(video_stream_->avg_frame_rate);
            tp += std::chrono::microseconds(duration);
            std::this_thread::sleep_until(tp);
        } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
        av_packet_free(&pkt);
        av_frame_free(&frame);

        // stop_request_ 为1时不需要回调
        if (!stop_request_ && ff_exception_callback_ && user_data_) {
            ff_exception_callback_(user_data_, user_handle_, ret, (uint8_t *) "video_decode_thread exited.");
        }
    }
    catch (const std::exception &e) {
        LOG_ERROR << "video thread exception(" << e.what() << ")";
    }
}

int FfmpegWrapper::input_interrupt_cb(void *ctx) {
    time_t preTime = *((time_t *) ctx);
    time_t nowTime = time(nullptr);
    if (nowTime - preTime > 10) {
        LOG_ERROR << "input_interrupt_cb timeOut";
        return true;
    }
    return false;
}
