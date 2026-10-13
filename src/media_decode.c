#define _POSIX_C_SOURCE 200809L
#include "media_decode.h"
#include "control.h"
#include "media_artifacts.h"
#include "media_input.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *out,uint32_t value) {
    for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(value>>(i*8));
}
void sources_media_audio_free(SourcesMediaAudio *out) {
    if(out){free(out->wave);memset(out,0,sizeof(*out));}
}
static int supported_audio(enum AVCodecID id) {
    return id==AV_CODEC_ID_MP3||id==AV_CODEC_ID_AAC||id==AV_CODEC_ID_FLAC||
           id==AV_CODEC_ID_VORBIS||id==AV_CODEC_ID_OPUS||id==AV_CODEC_ID_PCM_S16LE||
           id==AV_CODEC_ID_PCM_S16BE||id==AV_CODEC_ID_PCM_S24LE||
           id==AV_CODEC_ID_PCM_S32LE||id==AV_CODEC_ID_PCM_F32LE;
}
typedef struct {
    SwrContext *resampler;
    AVChannelLayout layout;
    enum AVSampleFormat format;
    unsigned rate;
    int64_t next_input,output_tick;
    int started,probe,seeking;
    int64_t origin_us,start,end;
    size_t written;
    unsigned char *pcm;
    const SourcesConfig *cfg;
} Conversion;
static const char *capture(Conversion *c,const unsigned char *bytes,int samples) {
    const char *error=sources_interrupted(c->cfg);if(error)return error;
    if(samples<0||samples>65536)return "media_frame_limit";
    if(c->output_tick>INT64_MAX-samples)return "media_timeline_unsupported";
    if(c->probe&&c->output_tick+samples>c->end)return "media_timeline_unsupported";
    int64_t from=c->output_tick>c->start?c->output_tick:c->start;
    int64_t until=c->output_tick+samples<c->end?c->output_tick+samples:c->end;
    if(until>from) {
        if(from!=c->start+(int64_t)c->written)return "media_timeline_unsupported";
        size_t count=(size_t)(until-from);
        if(c->pcm)memcpy(c->pcm+c->written*2,bytes+(size_t)(from-c->output_tick)*2,count*2);
        c->written+=count;
    }
    c->output_tick+=samples;return NULL;
}
static const char *convert_frame(Conversion *c,const AVFrame *frame,AVRational time_base,
                                 unsigned char *temporary) {
    const char *error=sources_interrupted(c->cfg);if(error)return error;
    if(frame->sample_rate<8000||frame->sample_rate>192000||
       frame->ch_layout.nb_channels<1||frame->ch_layout.nb_channels>8||
       frame->nb_samples<1||frame->nb_samples>65536)return "media_frame_limit";
    if(frame->best_effort_timestamp==AV_NOPTS_VALUE)return "media_timeline_unsupported";
    int64_t tick=av_rescale_q(frame->best_effort_timestamp,time_base,(AVRational){1,frame->sample_rate});
    int64_t origin=av_rescale_q(c->origin_us,AV_TIME_BASE_Q,(AVRational){1,frame->sample_rate});
    if((origin>0&&tick<INT64_MIN+origin)||(origin<0&&tick>INT64_MAX+origin))return "media_timeline_unsupported";
    tick-=origin;
    if(tick<-(int64_t)frame->sample_rate||tick>14401LL*frame->sample_rate)return "media_timeline_unsupported";
    if(!c->started) {
        if(c->seeking&&tick>av_rescale_q(c->start,(AVRational){1,16000},
                (AVRational){1,frame->sample_rate}))return "invalid_range";
        c->rate=(unsigned)frame->sample_rate;c->format=(enum AVSampleFormat)frame->format;
        if(av_channel_layout_copy(&c->layout,&frame->ch_layout)<0)return "resource_limit";
        AVChannelLayout mono;av_channel_layout_default(&mono,1);
        int rc=swr_alloc_set_opts2(&c->resampler,&mono,AV_SAMPLE_FMT_S16,16000,
                                   &c->layout,c->format,frame->sample_rate,0,NULL);
        av_channel_layout_uninit(&mono);
        if(rc<0||swr_init(c->resampler)<0)return "media_decode_failed";
        int64_t aligned=tick;
        if(c->seeking) {
            unsigned a=(unsigned)frame->sample_rate,b=16000;
            while(b){unsigned remainder=a%b;a=b;b=remainder;}
            int64_t period=frame->sample_rate/(int)a;
            int64_t padding=(tick%period+period)%period;
            /* Native resampling must retain the source-wide output grid after
             * seeking. At most one phase period of discarded preroll silence
             * aligns the input grid; no selected samples are synthesized. */
            if(padding&&swr_inject_silence(c->resampler,(int)padding)<0)
                return "media_decode_failed";
            aligned-=padding;
        }
        c->output_tick=av_rescale_q(aligned,(AVRational){1,frame->sample_rate},(AVRational){1,16000});
        c->next_input=tick;c->started=1;
    }
    if(c->rate!=(unsigned)frame->sample_rate||c->format!=frame->format||
       av_channel_layout_compare(&c->layout,&frame->ch_layout))return "media_format_changed";
    if(tick<c->next_input-2||tick>c->next_input+2)return "media_timeline_unsupported";
    if(tick>INT64_MAX-frame->nb_samples)return "media_timeline_unsupported";
    c->next_input=tick+frame->nb_samples;
    uint8_t *out=temporary;
    int samples=swr_convert(c->resampler,&out,65536,
                            (const uint8_t**)frame->extended_data,frame->nb_samples);
    return samples<0?"media_decode_failed":capture(c,temporary,samples);
}
static const char *audio_decode(const SourcesConfig *cfg,const void *bytes,
                                       size_t length,size_t start_ms,size_t end_ms,
                                       SourcesMediaAudio *result,int probe) {
    if(!result)return "invalid_request";
    memset(result,0,sizeof(*result));
    if(!bytes||!length||length>SOURCES_INPUT_MAX)return "source_too_large";
    if(end_ms<=start_ms||(!probe&&end_ms-start_ms>30000)||end_ms>14400000)return "invalid_range";
    const char *error=sources_interrupted(cfg);if(error)return error;
    SourcesMediaArtifacts artifacts;
    error=sources_media_artifacts_open(cfg,&artifacts,result->fingerprint);
    if(error)return error;
    SourcesMediaInput input={.bytes=bytes,.length=length,.cfg=cfg};
    unsigned char *buffer=av_malloc(32768),*temporary=NULL,*pcm=NULL;
    AVIOContext *io=NULL;AVFormatContext *format=NULL;AVCodecContext *codec=NULL;
    AVPacket *packet=NULL;AVFrame *frame=NULL;AVDictionary *options=NULL;
    Conversion conversion={.start=(int64_t)start_ms*16,.end=(int64_t)end_ms*16,.cfg=cfg,.probe=probe};
    if(!buffer)error="resource_limit";
    if(!error){io=avio_alloc_context(buffer,32768,0,&input,sources_media_read_memory,NULL,sources_media_seek_memory);if(!io)error="resource_limit";else buffer=NULL;}
    if(!error){format=avformat_alloc_context();if(!format)error="resource_limit";}
    if(!error) {
        format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->opaque=&input;
        format->io_open=sources_media_refuse_secondary;format->interrupt_callback=(AVIOInterruptCB){sources_media_interrupt,&input};
        format->max_streams=8;format->probesize=65536;format->max_analyze_duration=1000000;
        av_dict_set(&options,"format_whitelist","wav,mp3,mov,aac,flac,ogg,aiff",0);
        if(avformat_open_input(&format,NULL,NULL,&options)<0)error="invalid_media";
    }
    av_dict_free(&options);
    if(!error) {
        if(format->nb_streams>8)error="media_stream_limit";
        else {
            AVDictionary *stream_options[8]={0};
            unsigned count=format->nb_streams;
            for(unsigned i=0;i<count;i++) {
                av_dict_set(&stream_options[i],"threads","1",0);
                av_dict_set(&stream_options[i],"max_pixels","4194304",0);
                av_dict_set(&stream_options[i],"max_samples","65536",0);
            }
            int rc=avformat_find_stream_info(format,stream_options);
            for(unsigned i=0;i<count;i++)av_dict_free(&stream_options[i]);
            if(rc<0)error="invalid_media";
        }
    }
    unsigned selected=UINT_MAX;
    if(!error) {
        if(format->nb_streams>8||av_dict_count(format->metadata)>64)error="media_stream_limit";
        else if(format->duration==AV_NOPTS_VALUE||format->duration<=0||format->duration>14400LL*AV_TIME_BASE)error="media_timeline_unsupported";
        else {
            for(unsigned i=0;i<format->nb_streams;i++)if(format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){selected=i;break;}
            if(selected==UINT_MAX)error="media_audio_unavailable";
        }
    }
    if(!error) {
        AVStream *stream=format->streams[selected];AVCodecParameters *parameters=stream->codecpar;
        if(!supported_audio(parameters->codec_id))error="unsupported_audio_format";
        else {
            const AVCodec *decoder=avcodec_find_decoder(parameters->codec_id);
            codec=decoder?avcodec_alloc_context3(decoder):NULL;
            if(!codec)error="media_decoder_unavailable";
            else if(avcodec_parameters_to_context(codec,parameters)<0)error="invalid_media";
            else {codec->thread_count=1;codec->max_samples=65536;codec->max_pixels=4194304;
                codec->pkt_timebase=stream->time_base;
                if(avcodec_open2(codec,decoder,NULL)<0)error="media_decode_failed";}
        }
        conversion.origin_us=format->start_time==AV_NOPTS_VALUE?0:format->start_time;
    }
    if(!error&&!probe&&start_ms>=30000) {
        /* The existing native decoder handles codec preroll. Two seconds of
         * discarded input settle codec history and the finite resampling filter. */
        int64_t offset=(int64_t)(start_ms-2000)*1000;
        if(conversion.origin_us>INT64_MAX-offset)error="media_timeline_unsupported";
        else {
            int64_t target=av_rescale_q(conversion.origin_us+offset,AV_TIME_BASE_Q,
                format->streams[selected]->time_base);
            if(av_seek_frame(format,(int)selected,target,AVSEEK_FLAG_BACKWARD)<0)
                error="media_timeline_unsupported";
            else {avcodec_flush_buffers(codec);conversion.seeking=1;}
        }
    }
    if(!error) {
        packet=av_packet_alloc();frame=av_frame_alloc();temporary=malloc(65536*2);
        if(!probe)pcm=malloc((end_ms-start_ms)*32);
        if(!packet||!frame||!temporary||(!probe&&!pcm))error="resource_limit";
        conversion.pcm=pcm;
    }
    unsigned packets=0,packet_limit=probe?1000000:50000;int eof=0;
    while(!error&&(probe||conversion.written<(end_ms-start_ms)*16)) {
        error=sources_interrupted(cfg);if(error)break;
        if(++packets>packet_limit){error="media_packet_limit";break;}
        int rc=av_read_frame(format,packet);
        if(rc==AVERROR_EOF)eof=1;
        else if(rc<0){error="media_decode_failed";break;}
        if(!eof&&packet->size>1048576){error="media_packet_limit";break;}
        if(eof||packet->stream_index==(int)selected) {
            rc=avcodec_send_packet(codec,eof?NULL:packet);
            if(rc<0){error="media_decode_failed";break;}
            while((rc=avcodec_receive_frame(codec,frame))>=0) {
                error=convert_frame(&conversion,frame,format->streams[selected]->time_base,temporary);
                av_frame_unref(frame);if(error)break;
            }
            if(!error&&rc!=AVERROR(EAGAIN)&&rc!=AVERROR_EOF)error="media_decode_failed";
        }
        av_packet_unref(packet);
        if(eof) {
            if(!error&&conversion.started) {
                for(unsigned flushed=0;flushed<8&&(probe||conversion.written<(end_ms-start_ms)*16);flushed++) {
                    uint8_t *out=temporary;int n=swr_convert(conversion.resampler,&out,65536,NULL,0);
                    if(n<0){error="media_decode_failed";break;}
                    if(!n)break;
                    error=capture(&conversion,temporary,n);if(error)break;
                }
            }
            break;
        }
    }
    if(!error&&(probe?(!eof||conversion.written<16):conversion.written!=(end_ms-start_ms)*16))error="invalid_range";
    if(input.secondary_refused)error="media_external_reference_refused";
    const char *stopped=sources_interrupted(cfg);if(stopped)error=stopped;
    if(!error) {
        result->frames=conversion.written;result->stream=selected;
        result->source_rate=conversion.rate;result->source_channels=(unsigned)conversion.layout.nb_channels;
        result->origin_us=(long long)conversion.origin_us;
    }
    if(!error&&!probe) {
        size_t n=(end_ms-start_ms)*32;unsigned char *wave=calloc(1,n+44);
        if(!wave)error="resource_limit";
        else {
            memcpy(wave,"RIFF",4);put32(wave+4,(uint32_t)n+36);memcpy(wave+8,"WAVEfmt ",8);
            put32(wave+16,16);wave[20]=1;wave[22]=1;put32(wave+24,16000);put32(wave+28,32000);
            wave[32]=2;wave[34]=16;memcpy(wave+36,"data",4);put32(wave+40,(uint32_t)n);memcpy(wave+44,pcm,n);
            result->wave=wave;result->wave_bytes=n+44;
        }
    }
    swr_free(&conversion.resampler);av_channel_layout_uninit(&conversion.layout);
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&codec);
    avformat_close_input(&format);if(io)av_freep(&io->buffer);avio_context_free(&io);
    av_free(buffer);free(pcm);free(temporary);
    if(!error)error=sources_media_artifacts_check(cfg,&artifacts);
    sources_media_artifacts_close(&artifacts);
    if(error)sources_media_audio_free(result);
    return error;
}
const char *sources_media_audio_decode(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,SourcesMediaAudio *result) {
    return audio_decode(cfg,bytes,length,start,end,result,0);
}
const char *sources_media_audio_probe(const SourcesConfig *cfg,const void *bytes,
    size_t length,SourcesMediaAudio *result) {
    return audio_decode(cfg,bytes,length,0,14400000,result,1);
}
