/* Native fixture only: FFmpeg decodes vp08 entries but its MOV/MP4 muxer refuses
 * VP8. Remux actual native-encoded VP8 packets through a VP9 MP4 sample-table shell,
 * then restore the vp08 entry. No product decoder or codec bits are replaced. */
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==3);AVFormatContext *input=NULL,*output=NULL;
    assert(avformat_open_input(&input,argv[1],NULL,NULL)>=0);
    assert(avformat_find_stream_info(input,NULL)>=0&&input->nb_streams==1);
    AVStream *source=input->streams[0];assert(source->codecpar->codec_id==AV_CODEC_ID_VP8);
    assert(avformat_alloc_output_context2(&output,NULL,"mp4",argv[2])>=0&&output);
    output->flags&=~AVFMT_FLAG_AUTO_BSF;
    AVStream *stream=avformat_new_stream(output,NULL);assert(stream);
    assert(avcodec_parameters_copy(stream->codecpar,source->codecpar)>=0);
    stream->codecpar->codec_id=AV_CODEC_ID_VP9;stream->codecpar->codec_tag=0;
    stream->codecpar->profile=0;stream->codecpar->level=10;
    stream->codecpar->format=AV_PIX_FMT_YUV420P;stream->codecpar->bits_per_raw_sample=8;
    stream->time_base=source->time_base;
    assert(avio_open(&output->pb,argv[2],AVIO_FLAG_WRITE)>=0);
    assert(avformat_write_header(output,NULL)>=0);AVPacket *packet=av_packet_alloc();assert(packet);
    unsigned count=0;int rc;
    while((rc=av_read_frame(input,packet))>=0) {
        assert(++count<=1000&&packet->stream_index==0);
        av_packet_rescale_ts(packet,source->time_base,stream->time_base);
        packet->stream_index=0;packet->pos=-1;assert(av_interleaved_write_frame(output,packet)>=0);
        av_packet_unref(packet);
    }
    assert(rc==AVERROR_EOF&&count);assert(av_write_trailer(output)>=0);
    av_packet_free(&packet);avformat_close_input(&input);
    assert(avio_closep(&output->pb)>=0);avformat_free_context(output);
    FILE *file=fopen(argv[2],"r+b");assert(file);assert(!fseek(file,0,SEEK_END));
    long size=ftell(file);assert(size>0&&size<67108864);rewind(file);
    unsigned char *bytes=malloc((size_t)size);assert(bytes);
    assert(fread(bytes,1,(size_t)size,file)==(size_t)size);unsigned matches=0;
    for(long i=4;i+86<size;i++)if(!memcmp(bytes+i,"vp09",4)) {
        unsigned atom=(unsigned)bytes[i-4]<<24|(unsigned)bytes[i-3]<<16|
            (unsigned)bytes[i-2]<<8|bytes[i-1];
        if(atom>=86&&atom<=(unsigned)(size-i+4)) {
            assert(!fseek(file,i+3,SEEK_SET)&&fputc('8',file)!=EOF);matches++;
        }
    }
    assert(matches==1&&!fclose(file));free(bytes);return 0;
}
