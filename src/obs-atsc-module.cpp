#include <obs-module.h>
#include <../UI/obs-frontend-api/obs-frontend-api.h>
#include <obs.hpp>
#include <util/util.hpp>
#include <util/platform.h>
#include <util/threading.h>

#include <QMainWindow>
#include <QAction>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <memory>
#include <functional>

#include "obs-atsc-module-ui.h"

#include "atsc/atsc.h"
#include "atsc/atsc_parameters.h"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

template<typename T>
using unique_freeable_ptr = std::unique_ptr<T,std::function<void(T*)>>;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-atsc", "en-US")

static ATSCOutputUI *g_atsc_ui;

static bool g_output_running = false;
static obs_output_t *g_output;

struct atsc_output;
static atsc_output* g_atsc = nullptr;

struct atsc_output {

    atsc_output(obs_data_t *settings, obs_output_t *output) :
        fmt_ctx(nullptr),
        output(output)
    {
        enable_sdr = true;
        running = true;
        os_event_init(&event, OS_EVENT_TYPE_AUTO);
    	pthread_mutex_init(&lock, NULL);
        da_init(packets);

        pthread_create(&thread, NULL, _xmit_thread, this);

        auto driver_str = obs_data_get_string(settings, "sdr");
        auto gain = obs_data_get_double(settings, "rf_gain");
        auto freq_str = obs_data_get_string(settings, "rf_freq");
        auto freq = atof(freq_str);

        if (enable_sdr) {
            try {
                device = unique_freeable_ptr<SoapySDR::Device>(SoapySDR::Device::make(driver_str), [](SoapySDR::Device* d) { SoapySDR::Device::unmake(d); });
                device->setBandwidth(SOAPY_SDR_TX, 0, 6e6);
                device->setSampleRate(SOAPY_SDR_TX, 0, 4500000.0 / 286 * 684);
                device->setFrequencyCorrection(SOAPY_SDR_TX, 0, 0);
                device->setFrequency(SOAPY_SDR_TX, 0, freq);
                device->setGain(SOAPY_SDR_TX, 0, gain);

                stream = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0});
                samples = device->getStreamMTU(stream);

                result = unique_freeable_ptr<int16_t>((int16_t*)_mm_malloc(sizeof(int16_t) * atsc_parameters::ATSC_SYMBOLS_PER_FIELD, 32), [](int16_t* p) {
                    _mm_free(p);
                });

            } catch(...) {
                enable_sdr = false;
            }
        }
        enabled = enable_sdr;

        encoder = atsc_encoder::create();

        int ret = avformat_alloc_output_context2(&fmt_ctx, NULL, "mpegts", NULL);

        av_opt_set_int(fmt_ctx, "muxrate", 19393000, AV_OPT_SEARCH_CHILDREN);

        auto atsc_name = obs_data_get_string(settings, "atsc_name");
        auto atsc_channel_mj = obs_data_get_int(settings, "atsc_channel_mj");
        auto atsc_channel_mn = obs_data_get_int(settings, "atsc_channel_mn");
        
        char atsc_channel[20];
        snprintf(atsc_channel, 20, "%lld.%lld", atsc_channel_mj, atsc_channel_mn);

        av_dict_set(&fmt_ctx->metadata, "atsc_channel", atsc_channel, 0);
        av_dict_set(&fmt_ctx->metadata, "atsc_name", atsc_name, 0);

        fmt_ctx->max_delay = 100000;

        if (ret != 0) {
            enabled = false;
        }


    }
    ~atsc_output() {
        running = false;
        os_event_signal(event);
        pthread_join(thread, 0);
        printf("xmit thread stopped\n");
        avformat_free_context(fmt_ctx);
    }

    AVFormatContext* fmt_ctx;
    bool enabled;
    bool enable_sdr;
    int ts_fd;

    void encode(uint8_t *buf, int buf_size) {
        assert((buf_size % atsc_parameters::ATSC_MPEG2_BYTES) == 0);
        encoder->process(buf, buf_size / atsc_parameters::ATSC_MPEG2_BYTES, [this](void* data, unsigned sz) {

            int flags = 0;
            (void)sz;

            std::complex<float> *xfer = (std::complex<float>*)data;
            auto remaining = size_t(atsc_parameters::ATSC_SYMBOLS_PER_FIELD);
            while (remaining > 0) {
                auto transfer = std::min(remaining, samples);

                void* buffers[] = { xfer };
                device->writeStream(stream, buffers, transfer, flags);

                remaining -= transfer;
                xfer += transfer;
            }
        });


    }

    void change_gain(double gain) {
        device->setGain(SOAPY_SDR_TX, 0, gain);
    }

    void add_packet(struct encoder_packet *packet) {
        bool should_do_work;
    	struct encoder_packet new_packet;
        
        obs_encoder_packet_ref(&new_packet, packet);

        pthread_mutex_lock(&lock);
        da_push_back(packets, &new_packet);
        should_do_work = packets.num >= 50;  // WAG
        pthread_mutex_unlock(&lock);

        if (should_do_work) {
            os_event_signal(event);
        }
    }

private:
    static void* _xmit_thread(void* ptr) {
        atsc_output* self = (atsc_output*)ptr;
        self->xmit_thread();
        return NULL;
    }

    void xmit_thread() {
        DARRAY(struct encoder_packet) in_progress;
        da_init(in_progress);

        while (running) {
            os_event_wait(event);
        	pthread_mutex_lock(&lock);
            da_move(in_progress, packets);
            pthread_mutex_unlock(&lock);

            for (size_t i = 0; i < in_progress.num; i++) {
                struct encoder_packet *packet = &in_progress.array[i];

                AVPacket pkt;
                av_init_packet(&pkt);
                memset(&pkt, 0, sizeof(pkt));

                pkt.data = packet->data;
                pkt.size = packet->size;
                pkt.flags = packet->keyframe ? AV_PKT_FLAG_KEY : 0;
                pkt.stream_index = packet->type == OBS_ENCODER_VIDEO ? 0 : 1;

                pkt.pts = av_rescale_q_rnd(packet->pts, { packet->timebase_num, packet->timebase_den}, fmt_ctx->streams[pkt.stream_index]->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
                pkt.dts = av_rescale_q_rnd(packet->dts, { packet->timebase_num, packet->timebase_den}, fmt_ctx->streams[pkt.stream_index]->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

                uint8_t* sd = av_packet_new_side_data(&pkt, AV_PKT_DATA_MPEGTS_STREAM_ID, sizeof(uint8_t));
                *sd = pkt.stream_index;

                av_interleaved_write_frame(fmt_ctx, &pkt);
                obs_encoder_packet_release(packet);
            }
            da_free(in_progress);
        }

    }

    unique_freeable_ptr<SoapySDR::Device> device;
    std::unique_ptr<atsc_encoder> encoder;
    SoapySDR::Stream* stream;
    size_t samples;
    unique_freeable_ptr<int16_t> result;
    obs_output_t* output;

    pthread_t thread;
    pthread_mutex_t lock;
    os_event_t* event;
    bool running;
    DARRAY(struct encoder_packet) packets;

    static int atsc_output_packet(void *opaque, uint8_t *buf, int buf_size) {
        atsc_output* self = (atsc_output*)opaque;
        if (self->enable_sdr)
            self->encode(buf, buf_size);
        return 0;
    }

    static bool device_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings) {
        auto rf_list = obs_properties_get(props, "rf_freq");
        auto rf_gain = obs_properties_get(props, "rf_gain");

        obs_property_list_clear(rf_list);
        auto driver = obs_data_get_string(settings, "sdr");

        try {
            auto hw = SoapySDR::Device::make(driver);
            auto range = hw->getFrequencyRange(SOAPY_SDR_TX, 0, "RF");
            auto gainRange = hw->getGainRange(SOAPY_SDR_TX, 0);
            SoapySDR::Device::unmake(hw);

            obs_property_float_set_limits(rf_gain, gainRange.minimum(), gainRange.maximum(), 1.0);

            for (int i = 2; i < 70; i++) {
                //if (i == 37 or i == 54 or (i > 58 and i < 62) or (i > 64 and i < 67)) {
                //    continue;
                //}

                float freq;
                if (i < 5) {
                    freq = 57e6 + (i-2) * 6e6;
                } else if (i < 7) {
                    freq = 79e6 + (i-5) * 6e6;
                } else if (i < 14) {
                    freq = 177e6 + (i-7) * 6e6;
                } else {
                    freq = 473e6 + (i-14) * 6e6;
                }

                float freq_low = freq - 3e3;
                float freq_high = freq + 3e3;

                for (auto& r : range) {
                    if (r.minimum() <= freq_low and r.maximum() >= freq_high) {
                        char channel[4];
                        snprintf(channel, 4, "%d", i);

                        char frequency[40];
                        snprintf(frequency, 40, "%f", freq);
                        obs_property_list_add_string(rf_list, channel, frequency);
                    }
                }

            }

        } catch (...) {
        }

        return true;
    }

    static bool gain_changed(void* ctx, obs_properties_t *props, obs_property_t *property, obs_data_t *settings) {
        atsc_output* self = (atsc_output*)ctx;

        auto gain = obs_data_get_double(settings, "rf_gain");
        if (g_output_running) {
            self->change_gain(gain);
        }
        return false;
    }


private: /* obs interface */
    static const char* _obs_get_name(void*) {
        return obs_module_text("ATSC Encoder");
    }

    static void* _obs_create(obs_data_t *settings, obs_output_t *output) {
        g_atsc = new atsc_output(settings, output);
        if (!g_atsc->enabled) {
            delete g_atsc;
            g_atsc = nullptr;
        }
        return g_atsc;
    }

    static void _obs_destroy(void* ctx) {
        atsc_output* self = (atsc_output*)ctx;

        auto vcodec = obs_output_get_video_encoder(self->output);
        auto acodec = obs_output_get_audio_encoder(self->output, 0);
        obs_encoder_release(vcodec);
        obs_encoder_release(acodec);
        delete self;
    }

    static bool _obs_start(void* ctx) {
        atsc_output* self = (atsc_output*)ctx;

        auto props = obs_output_properties(self->output);
        //auto props = obs_get_output_properties("atsc-output");
        if (props != nullptr) {
            obs_property_set_enabled(obs_properties_get(props, "sdr"), false);
            obs_property_set_enabled(obs_properties_get(props, "sdr"), false);
            obs_property_set_enabled(obs_properties_get(props, "rf_freq"), false);
            obs_property_set_enabled(obs_properties_get(props, "atsc_channel_mj"), false);
            obs_property_set_enabled(obs_properties_get(props, "atsc_channel_mn"), false);
            obs_property_set_enabled(obs_properties_get(props, "atsc_name"), false);
            g_atsc_ui->RefreshProperties();
        }

        auto vcodec = obs_output_get_video_encoder(self->output);
        auto acodec = obs_output_get_audio_encoder(self->output, 0);

        size_t sz;
        AVCodecParameters* vparam;
        AVCodecParameters* aparam;
        
        obs_encoder_get_extra_data(vcodec, (uint8_t**)&vparam, &sz); assert(sz == sizeof(*vparam) && (sz = 0, true));
        obs_encoder_get_extra_data(acodec, (uint8_t**)&aparam, &sz); assert(sz == sizeof(*aparam) && (sz = 0, true));

        AVStream* stream;
        
        stream = avformat_new_stream(self->fmt_ctx, NULL);
        avcodec_parameters_copy(stream->codecpar, vparam);
        stream = avformat_new_stream(self->fmt_ctx, NULL);
        avcodec_parameters_copy(stream->codecpar, aparam);

        // FIXME: freeme
        uint8_t* buffer = (uint8_t*)av_malloc(188 * atsc_parameters::ATSC_DATA_SEGMENTS);
        self->fmt_ctx->pb = avio_alloc_context(buffer, 188 * atsc_parameters::ATSC_DATA_SEGMENTS, 1, ctx, NULL, atsc_output_packet, NULL);

        int ret = avformat_write_header(self->fmt_ctx, NULL); (void)ret;
        return true;
    }

    static void _obs_stop(void* ctx, uint64_t) {
        atsc_output* self = (atsc_output*)ctx;

        auto props = obs_output_properties(self->output);
        if (props != nullptr) {
            obs_property_set_enabled(obs_properties_get(props, "sdr"), true);
            obs_property_set_enabled(obs_properties_get(props, "sdr"), true);
            obs_property_set_enabled(obs_properties_get(props, "rf_freq"), true);
            obs_property_set_enabled(obs_properties_get(props, "atsc_channel_mj"), true);
            obs_property_set_enabled(obs_properties_get(props, "atsc_channel_mn"), true);
            obs_property_set_enabled(obs_properties_get(props, "atsc_name"), true);
            g_atsc_ui->RefreshProperties();
        }

        obs_output_end_data_capture(self->output);
    }

    static void _obs_data(void *ctx, struct encoder_packet *packet) {
        atsc_output* self = (atsc_output*)ctx;
        self->add_packet(packet);
    }

    static obs_properties_t * _obs_properties(void *ctx) {
        atsc_output* self = (atsc_output*)ctx;

        static obs_properties_t* props = nullptr;
        if (props != nullptr) {
            return props;
        }

        props = obs_properties_create();
        auto list = obs_properties_add_list(props, "sdr", "SDR Transmitter", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        for (auto device: SoapySDR::Device::enumerate()) {
            try {
                auto hw = SoapySDR::Device::make(device);
                auto can_xmit = hw->getNumChannels(SOAPY_SDR_TX) > 0;
                SoapySDR::Device::unmake(hw);

                if (can_xmit) {
                    obs_property_list_add_string(list, device["label"].c_str(), SoapySDR::KwargsToString(device).c_str());
                }
            } catch (...) {
                continue;
            }
        }
        obs_property_set_modified_callback(list, device_changed);

        list = obs_properties_add_list(props, "rf_freq", "RF Frequency", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        auto gain = obs_properties_add_float_slider(props, "rf_gain", "Gain", 0, 100, 1);
        obs_property_set_modified_callback2(gain, gain_changed, self);

        obs_properties_add_int(props, "atsc_channel_mj", "Channel", 1, 999, 1);
        obs_properties_add_int(props, "atsc_channel_mn", "Channel", 1, 99, 1);
        obs_properties_add_text(props, "atsc_name", "Station ID", OBS_TEXT_DEFAULT);
        obs_properties_add_bool(props, "auto_start", "Auto Start");
        return props;
    }

    static void _obs_update(void *data, obs_data_t *settings) {
    }

public:
    static struct obs_output_info output_info;

    static void defaults(obs_data_t *settings) {
        obs_data_set_double(settings, "rf_gain", 0);
        obs_data_set_int(settings, "atsc_channel_mj", 14);
        obs_data_set_int(settings, "atsc_channel_mn", 1);
        obs_data_set_string(settings, "atsc_name", "KOOL");
        obs_data_set_bool(settings, "auto_start", false);
    }

    static OBSData load_settings()
    {
        BPtr<char> path = obs_module_get_config_path(obs_current_module(),
                "obs-atsc.json");
        BPtr<char> jsonData = os_quick_read_utf8_file(path);
        if (!!jsonData) {
            obs_data_t *data = obs_data_create_from_json(jsonData);
            OBSData dataRet(data);
            obs_data_release(data);

            return dataRet;
        }

        obs_data_t* def = obs_data_create();
        defaults(def);
        return def;
    }


};



struct obs_output_info atsc_output::output_info = {
    .id                     = "atsc_output",
    .flags                  = OBS_OUTPUT_AV |
                              OBS_OUTPUT_ENCODED,
    .get_name               = atsc_output::_obs_get_name,
    .create                 = atsc_output::_obs_create,
    .destroy                = atsc_output::_obs_destroy,
    .start                  = atsc_output::_obs_start,
    .stop                   = atsc_output::_obs_stop,

    .encoded_packet         = atsc_output::_obs_data,

    .update                 = atsc_output::_obs_update,
    .get_properties         = atsc_output::_obs_properties,

    .encoded_video_codecs   = "mpeg2",
    .encoded_audio_codecs   = "ac3",
};

OBSData load_settings()
{
    return atsc_output::load_settings();
}


void output_start()
{
    if (!g_output_running) {
        OBSData settings = load_settings();
        if (settings != nullptr) {
            g_output = obs_output_create("atsc_output", 
                    "atsc_output", settings, NULL);
            if (g_atsc == nullptr) {
                obs_output_release(g_output);
                obs_data_release(settings);
                g_output = nullptr;
                return;
            }

            auto acodec = obs_audio_encoder_create("ffmpeg_ac3", "ac3", nullptr, 0, nullptr);
            obs_encoder_set_audio(acodec, obs_get_audio());
            obs_output_set_audio_encoder(g_output, acodec, 0);

            auto vcodec_settings = obs_data_create();
            obs_data_set_int(vcodec_settings, "qp", 4);
            auto vcodec = obs_video_encoder_create("ffmpeg_vaapi_mpeg2", "mpeg2", vcodec_settings, nullptr);
            obs_encoder_set_video(vcodec, obs_get_video());
            obs_output_set_video_encoder(g_output, vcodec);
            obs_data_release(vcodec_settings);

            obs_output_initialize_encoders(g_output, 0);
            obs_output_start(g_output);

            obs_output_begin_data_capture(g_output, 0);

            g_output_running = true;
        }
    }
}

void output_stop() {
    static int stopped = 0;
    if (g_output_running) {
        stopped++;
        obs_output_stop(g_output);
        obs_output_release(g_output);
        g_output_running = false;
    }
}

void addOutputUI(void)
{
	QAction *action = (QAction*)obs_frontend_add_tools_menu_qaction(
			obs_module_text("ATSC Output"));

	QMainWindow *window = (QMainWindow*)obs_frontend_get_main_window();

	obs_frontend_push_ui_translation(obs_module_get_string);
	g_atsc_ui = new ATSCOutputUI(window);
	obs_frontend_pop_ui_translation();

	auto cb = []() {
		g_atsc_ui->ShowHideDialog();
	};

	action->connect(action, &QAction::triggered, cb);
}


static void OBSEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		OBSData settings = load_settings();

		if (settings && obs_data_get_bool(settings, "auto_start"))
			output_start();
	}
}


extern struct obs_encoder_info vaapi_encoder_info;
extern struct obs_encoder_info ac3_encoder_info;

extern "C"
bool obs_module_load(void)
{
    obs_register_encoder(&ac3_encoder_info);
    obs_register_encoder(&vaapi_encoder_info);
    obs_register_output(&atsc_output::output_info);

	addOutputUI();
	obs_frontend_add_event_callback(OBSEvent, nullptr);

    return true;
}

extern "C"
void obs_module_unload(void)
{
}
