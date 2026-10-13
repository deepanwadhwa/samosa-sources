#include "media_video.h"
#include "media_input.h"
#include "media_artifacts.h"
#include "control.h"
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
/* Pixel coordinates after applying the container's orthogonal display matrix. */
static const int transforms[8][4]={
    {1,0,0,1},{0,1,-1,0},{-1,0,0,-1},{0,-1,1,0},
    {-1,0,0,1},{1,0,0,-1},{0,1,1,0},{0,-1,-1,0}
};
static const char *orientation(AVFrame *frame,AVStream *stream,unsigned *out) {
    const unsigned char *data=NULL;size_t length=0;
    AVFrameSideData *side=av_frame_get_side_data(frame,AV_FRAME_DATA_DISPLAYMATRIX);
    if(side){data=side->data;length=side->size;}
    else {
#if LIBAVFORMAT_VERSION_MAJOR >= 61
        AVCodecParameters *parameters=stream->codecpar;
        for(int i=0;i<parameters->nb_coded_side_data;i++)
            if(parameters->coded_side_data[i].type==AV_PKT_DATA_DISPLAYMATRIX) {
                data=parameters->coded_side_data[i].data;
                length=parameters->coded_side_data[i].size;break;
            }
#else
        data=av_stream_get_side_data(stream,AV_PKT_DATA_DISPLAYMATRIX,&length);
#endif
    }
    *out=0;if(!data)return NULL;
    int32_t matrix[9];
    if(length!=sizeof(matrix))return "media_orientation_unsupported";
    memcpy(matrix,data,sizeof(matrix));
    if(matrix[2]||matrix[5]||matrix[8]!=(1<<30))return "media_orientation_unsupported";
    for(unsigned i=0;i<8;i++) {
        const int *t=transforms[i];
        if(matrix[0]==t[0]*65536&&matrix[1]==t[1]*65536&&
           matrix[3]==t[2]*65536&&matrix[4]==t[3]*65536){*out=i;return NULL;}
    }
    return "media_orientation_unsupported";
}
static const char *orient_pixels(const SourcesConfig *cfg,unsigned code,
    unsigned char **pixels,unsigned *width,unsigned *height) {
    if(!code)return NULL;
    unsigned w=*width,h=*height;
    const int *t=transforms[code];
    unsigned ow=t[0]?w:h,oh=t[1]?w:h;
    unsigned char *result=malloc((size_t)w*h*4);
    if(!result)return "resource_limit";
    for(unsigned y=0;y<h;y++) {
        const char *error=sources_interrupted(cfg);
        if(error){free(result);return error;}
        for(unsigned x=0;x<w;x++) {
            int dx=t[0]*(int)x+t[2]*(int)y;
            int dy=t[1]*(int)x+t[3]*(int)y;
            if(t[0]<0)dx+=(int)w-1;
            if(t[2]<0)dx+=(int)h-1;
            if(t[1]<0)dy+=(int)w-1;
            if(t[3]<0)dy+=(int)h-1;
            memcpy(result+((size_t)dy*ow+(unsigned)dx)*4,
                *pixels+((size_t)y*w+x)*4,4);
        }
    }
    free(*pixels);*pixels=result;*width=ow;*height=oh;return NULL;
}
void sources_media_frame_free(SourcesMediaFrame *frame) {
    if(frame){free(frame->rgba);memset(frame,0,sizeof(*frame));}
}
/* Reuse native color/tonemap filters, never a hand-written transfer function.
 * The fixed peak makes the transformation independent of optional scene metadata. */
static const char *hdr_pixels(const SourcesConfig *cfg,AVFrame *frame,unsigned width,
    unsigned height,unsigned char **pixels) {
    *pixels=NULL;const char *error=sources_interrupted(cfg);if(error)return error;
    if(frame->color_primaries!=AVCOL_PRI_BT2020||frame->colorspace!=AVCOL_SPC_BT2020_NCL)
        return "media_visual_format_unsupported";
    AVFilterGraph *graph=avfilter_graph_alloc();AVFilterContext *source=NULL,*sink=NULL;
    AVFilterInOut *inputs=avfilter_inout_alloc(),*outputs=avfilter_inout_alloc();
    AVFrame *converted=av_frame_alloc();char arguments[192],filters[768];
    if(!graph||!inputs||!outputs||!converted)error="resource_limit";
    const char *transfer=frame->color_trc==AVCOL_TRC_SMPTE2084?"smpte2084":"arib-std-b67";
    if(!error){
        graph->nb_threads=1;
        snprintf(arguments,sizeof(arguments),"video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",frame->width,frame->height,frame->format);
        if(avfilter_graph_create_filter(&source,avfilter_get_by_name("buffer"),"in",arguments,NULL,graph)<0||
           avfilter_graph_create_filter(&sink,avfilter_get_by_name("buffersink"),"out",NULL,NULL,graph)<0)error="media_visual_format_unsupported";
        if(avfilter_get_by_name("zscale"))
            snprintf(filters,sizeof(filters),"zscale=w=%u:h=%u:filter=bilinear:tin=%s:pin=bt2020:min=bt2020nc:t=linear:p=bt709:r=full:npl=100,format=gbrpf32le,tonemap=tonemap=hable:desat=0:peak=10,zscale=t=iec61966-2-1:p=bt709:m=bt709:r=full,format=rgba",width,height,transfer);
        else
            snprintf(filters,sizeof(filters),"scale=w=%u:h=%u:flags=bilinear:in_transfer=%s:out_transfer=linear:in_primaries=bt2020:out_primaries=bt709:in_color_matrix=bt2020:out_range=full,format=gbrpf32le,tonemap=tonemap=hable:desat=0:peak=10,scale=in_transfer=linear:out_transfer=iec61966-2-1:in_primaries=bt709:out_primaries=bt709:out_range=full,format=rgba",width,height,transfer);
    }
    if(!error){
        outputs->name=av_strdup("in");outputs->filter_ctx=source;outputs->pad_idx=0;
        inputs->name=av_strdup("out");inputs->filter_ctx=sink;inputs->pad_idx=0;
        if(!outputs->name||!inputs->name)error="resource_limit";
        else if(avfilter_graph_parse_ptr(graph,filters,&inputs,&outputs,NULL)<0||avfilter_graph_config(graph,NULL)<0)
            error="media_visual_format_unsupported";
    }
    if(!error)error=sources_interrupted(cfg);
    if(!error&&(av_buffersrc_add_frame_flags(source,frame,AV_BUFFERSRC_FLAG_KEEP_REF)<0||
       av_buffersink_get_frame(sink,converted)<0))error="media_decode_failed";
    if(!error&&(converted->format!=AV_PIX_FMT_RGBA||converted->width!=(int)width||
       converted->height!=(int)height||converted->linesize[0]<(int)width*4))error="media_decode_failed";
    if(!error){
        *pixels=malloc((size_t)width*height*4);if(!*pixels)error="resource_limit";
        else for(unsigned row=0;row<height;row++){
            error=sources_interrupted(cfg);if(error)break;
            memcpy(*pixels+(size_t)row*width*4,converted->data[0]+(size_t)row*converted->linesize[0],(size_t)width*4);
        }
    }
    av_frame_free(&converted);avfilter_inout_free(&inputs);avfilter_inout_free(&outputs);avfilter_graph_free(&graph);
    if(error){free(*pixels);*pixels=NULL;}return error;
}
static const char *capture(const SourcesConfig *cfg,AVFrame *frame,
    AVStream *stream,int64_t origin,size_t start,size_t end,int64_t *previous,
    SourcesMediaFrame *out) {
    const char *error=sources_interrupted(cfg);if(error)return error;
    if(frame->width<1||frame->height<1||frame->width>4096||frame->height>4096||
       (uint64_t)frame->width*frame->height>4194304)return "media_frame_limit";
    if(frame->best_effort_timestamp==AV_NOPTS_VALUE)return "media_timeline_unsupported";
    int64_t absolute=av_rescale_q(frame->best_effort_timestamp,stream->time_base,AV_TIME_BASE_Q);
    if((origin>0&&absolute<INT64_MIN+origin)||(origin<0&&absolute>INT64_MAX+origin))
        return "media_timeline_unsupported";
    int64_t time=absolute-origin;
    if(time<0||time>14400000000LL||time<=*previous)return "media_timeline_unsupported";
    *previous=time;
    if(time<(int64_t)start*1000)return NULL;
    if(time>=(int64_t)end*1000)return "invalid_range";
    unsigned transform;
    error=orientation(frame,stream,&transform);if(error)return error;
    unsigned w=(unsigned)frame->width,h=(unsigned)frame->height;
    if(w>512||h>512){unsigned axis=w>h?w:h;w=w*512/axis;h=h*512/axis;if(!w)w=1;if(!h)h=1;}
    if(frame->color_trc<0||frame->color_trc>18||frame->color_primaries<0||frame->color_primaries>22)
        return "media_visual_format_unsupported";
    int hdr=frame->color_trc==AVCOL_TRC_SMPTE2084||frame->color_trc==AVCOL_TRC_ARIB_STD_B67;
    if(hdr){
        unsigned char *rgba=NULL;error=hdr_pixels(cfg,frame,w,h,&rgba);if(error)return error;
        error=orient_pixels(cfg,transform,&rgba,&w,&h);if(error){free(rgba);return error;}
        out->rgba=rgba;out->bytes=(size_t)w*h*4;out->width=w;out->height=h;out->orientation=transform;
        out->source_width=(unsigned)frame->width;out->source_height=(unsigned)frame->height;
        out->timestamp_us=time;out->origin_us=origin;out->colorspace=SWS_CS_BT2020;out->color_range=frame->color_range==AVCOL_RANGE_JPEG;
        out->source_transfer=(unsigned)frame->color_trc;out->source_primaries=(unsigned)frame->color_primaries;
        out->output_transfer=AVCOL_TRC_IEC61966_2_1;out->output_primaries=AVCOL_PRI_BT709;out->tone_mapped=1;return NULL;
    }
    struct SwsContext *scale=sws_getContext(frame->width,frame->height,frame->format,
        (int)w,(int)h,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);
    if(!scale)return "media_visual_format_unsupported";
    int matrix=frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:
               frame->colorspace==AVCOL_SPC_BT2020_NCL?SWS_CS_BT2020:SWS_CS_ITU601;
    int full=frame->color_range==AVCOL_RANGE_JPEG;
    if(sws_setColorspaceDetails(scale,sws_getCoefficients(matrix),full,
        sws_getCoefficients(matrix),1,0,1<<16,1<<16)<0){sws_freeContext(scale);return "media_visual_format_unsupported";}
    size_t bytes=(size_t)w*h*4;unsigned char *rgba=malloc(bytes);
    if(!rgba){sws_freeContext(scale);return "resource_limit";}
    uint8_t *planes[4]={rgba,NULL,NULL,NULL};int strides[4]={(int)w*4,0,0,0};
    int rows=sws_scale(scale,(const uint8_t *const*)frame->data,frame->linesize,0,
        frame->height,planes,strides);sws_freeContext(scale);
    error=sources_interrupted(cfg);
    if(error||rows!=(int)h){free(rgba);return error?error:"media_decode_failed";}
    error=orient_pixels(cfg,transform,&rgba,&w,&h);
    if(error){free(rgba);return error;}
    out->rgba=rgba;out->bytes=bytes;out->width=w;out->height=h;
    out->orientation=transform;
    out->source_width=(unsigned)frame->width;out->source_height=(unsigned)frame->height;
    out->timestamp_us=time;out->origin_us=origin;out->colorspace=matrix;out->color_range=full;
    out->source_transfer=out->output_transfer=(unsigned)frame->color_trc;
    out->source_primaries=out->output_primaries=(unsigned)frame->color_primaries;
    return NULL;
}
static const char *video_read(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,SourcesMediaFrame *out, SourcesMediaVideoInfo *info,int last) {
    if(!out)return "invalid_request";
    memset(out,0,sizeof(*out));
    if(!bytes||!length||length>SOURCES_INPUT_MAX)return "source_too_large";
    if(end<=start||end-start>30000||end>14400000)return "invalid_range";
    SourcesMediaArtifacts artifacts;
    const char *error=sources_media_artifacts_open(cfg,&artifacts,out->fingerprint);
    if(error)return error;
    SourcesMediaInput input={.bytes=bytes,.length=length,.cfg=cfg};
    unsigned char *buffer=av_malloc(32768);AVIOContext *io=NULL;
    AVFormatContext *format=NULL;AVCodecContext *codec=NULL;
    AVPacket *packet=NULL;AVFrame *frame=NULL;AVDictionary *options=NULL;
    if(!buffer)error="resource_limit";
    if(!error){io=avio_alloc_context(buffer,32768,0,&input,sources_media_read_memory,NULL,sources_media_seek_memory);if(!io)error="resource_limit";else buffer=NULL;}
    if(!error){format=avformat_alloc_context();if(!format)error="resource_limit";}
    if(!error){format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->opaque=&input;
        format->io_open=sources_media_refuse_secondary;
        format->interrupt_callback=(AVIOInterruptCB){sources_media_interrupt,&input};
        format->max_streams=8;format->probesize=65536;format->max_analyze_duration=1000000;
        av_dict_set(&options,"format_whitelist","mov",0);
        if(avformat_open_input(&format,NULL,NULL,&options)<0)error="invalid_media";}
    av_dict_free(&options);
    if(!error) {
        if(format->nb_streams>8)error="media_stream_limit";
        else {
            AVDictionary *probe[8]={0};unsigned count=format->nb_streams;
            for(unsigned i=0;i<count;i++) {
                av_dict_set(&probe[i],"threads","1",0);
                av_dict_set(&probe[i],"max_pixels","4194304",0);
                av_dict_set(&probe[i],"max_samples","65536",0);
            }
            int rc=avformat_find_stream_info(format,probe);
            for(unsigned i=0;i<count;i++)av_dict_free(&probe[i]);
            if(rc<0)error="invalid_media";
        }
    }
    unsigned selected=UINT_MAX;
    if(!error){if(format->nb_streams>8||av_dict_count(format->metadata)>64)error="media_stream_limit";
        else for(unsigned i=0;i<format->nb_streams;i++)if(format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){selected=i;break;}
        if(!error&&selected==UINT_MAX)error="media_video_unavailable";
        if(!error&&(format->duration==AV_NOPTS_VALUE||format->duration<=0||
                    format->duration>14400000000LL))error="media_timeline_unsupported";}
    if(!error){AVCodecParameters *p=format->streams[selected]->codecpar;
        if(p->width<1||p->height<1||p->width>4096||p->height>4096||(uint64_t)p->width*p->height>4194304)error="media_frame_limit";
        else if(p->codec_id!=AV_CODEC_ID_H264&&p->codec_id!=AV_CODEC_ID_HEVC&&
                p->codec_id!=AV_CODEC_ID_MPEG4&&p->codec_id!=AV_CODEC_ID_MJPEG&&
                p->codec_id!=AV_CODEC_ID_VP8&&p->codec_id!=AV_CODEC_ID_VP9)
            error="media_visual_format_unsupported";
        else if(!info) {const AVCodec *decoder=avcodec_find_decoder(p->codec_id);
            codec=decoder?avcodec_alloc_context3(decoder):NULL;
            if(!codec)error="media_decoder_unavailable";
            else if(avcodec_parameters_to_context(codec,p)<0)error="invalid_media";
            else {codec->thread_count=1;codec->max_pixels=4194304;codec->max_samples=65536;
                codec->pkt_timebase=format->streams[selected]->time_base;
                if(avcodec_open2(codec,decoder,NULL)<0)error="media_decode_failed";}}}
    int64_t previous=-1,origin=!error&&format->start_time!=AV_NOPTS_VALUE?format->start_time:0;
    /* Preserve the monolith's selected-time decoding: seek to a preceding
     * keyframe for late selections instead of spending the packet budget on
     * unrelated earlier media. The decoder still validates every returned frame. */
    if(!error&&!info&&start>=30000) {
        if(origin>INT64_MAX-(int64_t)start*1000)error="media_timeline_unsupported";
        else {
            int64_t target=av_rescale_q(origin+(int64_t)start*1000,AV_TIME_BASE_Q,
                format->streams[selected]->time_base);
            if(av_seek_frame(format,(int)selected,target,AVSEEK_FLAG_BACKWARD)<0)
                error="media_timeline_unsupported";
            else avcodec_flush_buffers(codec);
        }
    }
    if(!error&&!info){packet=av_packet_alloc();frame=av_frame_alloc();if(!packet||!frame)error="resource_limit";}
    int finished=0;
    for(unsigned count=0;!error&&!info&&!finished&&(!out->rgba||last);count++) {
        if(count>=50000){error="media_packet_limit";break;}
        error=sources_interrupted(cfg);if(error)break;
        int rc=av_read_frame(format,packet),eof=rc==AVERROR_EOF;
        if(rc<0&&!eof){error="media_decode_failed";break;}
        if(!eof&&(packet->size<0||packet->size>1048576)){error="media_packet_limit";break;}
        if(eof||packet->stream_index==(int)selected) {
            rc=avcodec_send_packet(codec,eof?NULL:packet);
            if(rc<0&&rc!=AVERROR_EOF){error="media_decode_failed";break;}
            unsigned frames=0;
            while((rc=avcodec_receive_frame(codec,frame))>=0) {
                if(++frames>256){error="media_frame_limit";break;}
                SourcesMediaFrame candidate={0};
                error=capture(cfg,frame,format->streams[selected],origin,start,end,&previous,last?&candidate:out);
                if(last&&error&&!strcmp(error,"invalid_range")&&out->rgba){error=NULL;finished=1;}
                if(last&&candidate.rgba) {
                    memcpy(candidate.fingerprint,out->fingerprint,65);
                    free(out->rgba);*out=candidate;
                }
                av_frame_unref(frame);if(error||finished||(!last&&out->rgba))break;
            }
            if(!error&&!finished&&(!out->rgba||last)&&rc!=AVERROR(EAGAIN)&&rc!=AVERROR_EOF)error="media_decode_failed";
        }
        av_packet_unref(packet);if(eof)break;
    }
    if(!error&&!info&&!out->rgba)error="invalid_range";
    if(input.secondary_refused)error="media_external_reference_refused";
    if(!error){out->stream=selected;error=sources_media_artifacts_check(cfg,&artifacts);}
    if(!error&&info) {
        info->duration_ms=(size_t)(format->duration/1000);
        if(!info->duration_ms)error="media_timeline_unsupported";
        else {info->stream=selected;info->source_width=(unsigned)format->streams[selected]->codecpar->width;
            info->source_height=(unsigned)format->streams[selected]->codecpar->height;
            info->origin_us=origin;memcpy(info->fingerprint,out->fingerprint,65);}
    }
    const char *stopped=sources_interrupted(cfg);if(stopped)error=stopped;
    av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&codec);
    avformat_close_input(&format);if(io)av_freep(&io->buffer);avio_context_free(&io);av_free(buffer);
    sources_media_artifacts_close(&artifacts);if(error)sources_media_frame_free(out);
    return error;
}

const char *sources_media_video_frame(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,SourcesMediaFrame *out) {
    return video_read(cfg,bytes,length,start,end,out,NULL,0);
}
const char *sources_media_video_last_frame(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,SourcesMediaFrame *out) {
    return video_read(cfg,bytes,length,start,end,out,NULL,1);
}
const char *sources_media_video_probe(const SourcesConfig *cfg,const void *bytes,
    size_t length,SourcesMediaVideoInfo *info) {
    if(!info)return "invalid_request";
    memset(info,0,sizeof(*info));
    SourcesMediaFrame frame={0};
    const char *error=video_read(cfg,bytes,length,0,1,&frame,info,0);
    if(error)memset(info,0,sizeof(*info));
    return error;
}
