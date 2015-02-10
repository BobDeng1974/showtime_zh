/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2013 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>

#include "fileaccess/fa_libav.h"
#include "media/media.h"
#include "backend/backend.h"
#include "misc/minmax.h"
#include "misc/str.h"
#include "misc/bytestream.h"

#include "hls.h"

#define PTS_MASK 0x1ffffffffLL

LIST_HEAD(ts_service_list, ts_service);
LIST_HEAD(ts_es_list, ts_es);

typedef struct ts_table {
  int tt_offset;
  int tt_lock;
  uint8_t *tt_data;
} ts_table_t;



static const AVRational mpeg_tc = {1, 90000};

typedef struct ts_demuxer {
  struct ts_service_list td_services;
  struct ts_es_list td_elemtary_streams;
  ts_table_t td_pat;
  media_pipe_t *td_mp;
  hls_demuxer_t *td_hd;

  struct media_buf_queue td_packets;

  enum {
    TD_MUX_MODE_UNSET,
    TD_MUX_MODE_TS,
    TD_MUX_MODE_RAW,
  } td_mux_mode;

  uint8_t td_buf[2048];
  int td_buf_bytes;

} ts_demuxer_t;


typedef struct ts_service {
  LIST_ENTRY(ts_service) tss_link;
  uint16_t tss_pmtpid;
  ts_table_t tss_pmt;
  ts_demuxer_t *tss_demuxer;
} ts_service_t;


typedef struct ts_es {
  // Elementary Stream
  LIST_ENTRY(ts_es) te_link;
  uint16_t te_pid;

  uint8_t *te_buf;
  int te_buf_size;
  int te_packet_size;

  int te_data_type;
  int te_probe_frame;
  int te_stream;

  char te_logged_ts;
  char te_logged_keyframe;

  int64_t te_pts;
  int64_t te_dts;
  int64_t te_bts; // Base timestamp

  media_codec_t *te_codec;

  // Use to derive timestamps for files without timestamps
  int te_samples_per_frame;
  int te_sample_rate;
  int64_t te_samples;

  int te_current_seq;

  int64_t te_ts_offset;
} ts_es_t;


/**
 *
 */
static void
td_flush_packets(ts_demuxer_t *td)
{
  media_buf_t *mb, *next;
  for(mb = TAILQ_FIRST(&td->td_packets); mb != NULL; mb = next) {
    next = TAILQ_NEXT(mb, mb_link);
    media_buf_free_unlocked(td->td_mp, mb);
  }
}


/**
 *
 */
static void
te_destroy(ts_es_t *te, media_pipe_t *mp)
{
  LIST_REMOVE(te, te_link);
  free(te->te_buf);
  if(te->te_codec != NULL)
    media_codec_deref(te->te_codec);
  free(te);
}


/**
 *
 */
static void
tss_destroy(ts_service_t *tss)
{
  LIST_REMOVE(tss, tss_link);
  free(tss->tss_pmt.tt_data);
  free(tss);
}


/**
 *
 */
static void
ts_demuxer_destroy(ts_demuxer_t *td)
{
  ts_es_t *te;
  ts_service_t *tss;

  td_flush_packets(td);

  while((te = LIST_FIRST(&td->td_elemtary_streams)) != NULL)
    te_destroy(te, td->td_mp);

  while((tss = LIST_FIRST(&td->td_services)) != NULL)
    tss_destroy(tss);

  free(td->td_pat.tt_data);

  free(td);
}

/**
 *
 */
static int
table_reassemble(ts_table_t *tt, const uint8_t *data, int len,
                 int pusi, void (*cb)(void *opaque, const uint8_t *data,
                                      int len), void *opaque)
{
  int excess, tsize;

  if(pusi) {
    tt->tt_offset = 0;
    tt->tt_lock = 1;
  }

  if(!tt->tt_lock)
    return -1;

  tt->tt_data = realloc(tt->tt_data, tt->tt_offset + len);
  memcpy(tt->tt_data + tt->tt_offset, data, len);
  tt->tt_offset += len;

  if(tt->tt_offset < 3)
    return len;

  tsize = 3 + (((tt->tt_data[1] & 0xf) << 8) | tt->tt_data[2]);

  if(tt->tt_offset < tsize)
    return len;

  excess = tt->tt_offset - tsize;

  tt->tt_offset = 0;
  if(tsize >= 7)
    cb(opaque, tt->tt_data + 3, tsize - 7);

  free(tt->tt_data);
  tt->tt_data = NULL;

  return len - excess;
}


/**
 *
 */
static void
parse_table(ts_table_t *tt, const uint8_t *tsb,
            void (*cb)(void *opaque, const uint8_t *data, int len),
            void *opaque)
{
  int off  = tsb[3] & 0x20 ? tsb[4] + 5 : 4;
  int pusi = tsb[1] & 0x40;

  if(off >= 188) {
    tt->tt_lock = 0;
    return;
  }

  if(pusi) {
    int len = tsb[off++];
    if(len > 0) {
      if(len > 188 - off) {
        tt->tt_lock = 0;
        return;
      }
      table_reassemble(tt, tsb + off, len, 0, cb, opaque);
      off += len;
    }
  }

  while(off < 188) {
    int r = table_reassemble(tt, tsb + off, 188 - off, pusi, cb, opaque);
    if(r < 0) {
      tt->tt_lock = 0;
      break;
    }
    off += r;
    pusi = 0;
  }
}


/**
 *
 */
static ts_service_t *
find_service(ts_demuxer_t *td, uint16_t pmtpid, int create)
{
  ts_service_t *tss;
  LIST_FOREACH(tss, &td->td_services, tss_link) {
    if(tss->tss_pmtpid == pmtpid)
      return tss;
  }
  if(create) {
    tss = calloc(1, sizeof(ts_service_t));
    tss->tss_demuxer = td;
    tss->tss_pmtpid = pmtpid;
    LIST_INSERT_HEAD(&td->td_services, tss, tss_link);
  }
  return tss;
}


/**
 *
 */
static ts_es_t *
find_es(ts_demuxer_t *td, uint16_t pid, int create)
{
  ts_es_t *te;
  LIST_FOREACH(te, &td->td_elemtary_streams, te_link) {
    if(te->te_pid == pid)
      return te;
  }
  if(create) {
    te = calloc(1, sizeof(ts_es_t));
    te->te_pid = pid;
    LIST_INSERT_HEAD(&td->td_elemtary_streams, te, te_link);
  }
  return te;
}


/**
 *
 */
static void
handle_pat(void *opaque, const uint8_t *ptr, int len)
{
  ts_demuxer_t *td = opaque;
  ptr += 5;
  len -= 5;
  while(len >= 4) {
    const uint16_t pid = (ptr[2] & 0x1f) << 8 | ptr[3];
    find_service(td, pid, 1);

    len -= 4;
    ptr += 4;
  }
}



/**
 *
 */
static void
handle_pmt(void *opaque, const uint8_t *ptr, int len)
{
  ts_service_t *tss = opaque;
  ts_demuxer_t *td = tss->tss_demuxer;
  hls_t *h = td->td_hd->hd_hls;
  int pid;
  int dllen;
  uint8_t dlen, estype;

  if(len < 9) {
    return;
  }

  //  service_id = ptr[0] << 8 | ptr[1];
  //  x          = (ptr[5] & 0x1f) << 8 | ptr[6];
  dllen      = (ptr[7] & 0xf) << 8 | ptr[8];

  ptr += 9;
  len -= 9;

  while(dllen > 1) {
    //dtag = ptr[0];

    dlen = ptr[1];
    len -= 2; ptr += 2; dllen -= 2;
    if(dlen > len) {
      return;
    }
    len -= dlen; ptr += dlen; dllen -= dlen;
  }

  while(len >= 5) {
    estype  = ptr[0];
    pid     = (ptr[1] & 0x1f) << 8 | ptr[2];
    dllen   = (ptr[3] & 0xf) << 8 | ptr[4];

    ptr += 5;
    len -= 5;

    while(dllen > 1) {
      //      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2;
      if(dlen > len)
	break;
      len -= dlen; ptr += dlen; dllen -= dlen;
    }

    ts_es_t *te = find_es(td, pid, 1);

    if(te->te_codec == NULL) {

      switch(estype) {
      case 0x1b:
        te->te_codec = media_codec_ref(td->td_hd->hd_hls->h_codec_h264);
        te->te_data_type = MB_VIDEO;
        te->te_stream = 0;
        break;

      case 0x0f:
        te->te_codec = media_codec_create(AV_CODEC_ID_AAC, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, pid, NULL, NULL, "AAC", 1);
        break;

      case 0x03:
      case 0x04:
        te->te_codec = media_codec_create(AV_CODEC_ID_MP3, 1, NULL, NULL, NULL,
                                          td->td_mp);
        te->te_data_type = MB_AUDIO;
        te->te_stream = hls_get_audio_track(h, pid, NULL, NULL, "MP3", 1);
        break;

      default:
        break;
      }
    }
  }
}

#define getu8(b, l) ({ \
  uint8_t x = b[0];    \
  b+=1;                \
  l-=1;                \
  x;                   \
})


static int64_t
getpts(const uint8_t *p)
{
  int a =  p[0];
  int b = (p[1] << 8) | p[2];
  int c = (p[3] << 8) | p[4];

  if((a & 1) && (b & 1) && (c & 1)) {

    return
      ((int64_t)((a >> 1) & 0x07) << 30) |
      ((int64_t)((b >> 1)       ) << 15) |
      ((int64_t)((c >> 1)       ))
      ;

  } else {
    // Marker bits not present
    return PTS_UNSET;
  }
}


/**
 *
 */
static int
parse_pes_header(ts_es_t *te, const uint8_t *buf, size_t len)
{
  int64_t d;
  int hdr, flags, hlen;

  hdr   = getu8(buf, len);
  flags = getu8(buf, len);
  hlen  = getu8(buf, len);

  te->te_pts = PTS_UNSET;
  te->te_dts = PTS_UNSET;

  if(len < hlen || (hdr & 0xc0) != 0x80)
    return -1;

  if((flags & 0xc0) == 0xc0) {
    if(hlen < 10)
      return -1;

    te->te_pts = getpts(buf);
    te->te_dts = getpts(buf + 5);

    d = (te->te_pts - te->te_dts) & PTS_MASK;
    if(d > 180000) // More than two seconds of PTS/DTS delta, probably corrupt
      te->te_dts = te->te_pts = PTS_UNSET;

  } else if((flags & 0xc0) == 0x80) {
    if(hlen < 5)
      return -1;

    te->te_dts = te->te_pts = getpts(buf);
  } else
    return hlen + 3;

  return hlen + 3;
}



/**
 *
 */
static int64_t
rescale(int64_t ts)
{

  if(ts == AV_NOPTS_VALUE)
    return AV_NOPTS_VALUE;

  return av_rescale_q(ts, mpeg_tc, AV_TIME_BASE_Q);
}


/**
 *
 */
static void
probe_duration(ts_es_t *te, uint8_t *data, int size)
{
  int got_frame = 0;
  AVPacket pkt = {
    .data = data,
    .size = size
  };


  media_codec_t *mc = te->te_codec;

  AVCodec *codec = avcodec_find_decoder(mc->codec_id);
  if(codec == NULL) {
    te->te_probe_frame = 0;
    return;
  }

  AVCodecContext *ctx = avcodec_alloc_context3(codec);

  if(avcodec_open2(ctx, codec, NULL) < 0) {
    av_freep(&ctx);
    te->te_probe_frame = 0;
    return;
  }

  AVFrame *frame = av_frame_alloc();

  avcodec_decode_audio4(ctx, frame, &got_frame, &pkt);

  if(got_frame) {
    te->te_probe_frame = 0;
    te->te_samples_per_frame = frame->nb_samples;
    te->te_sample_rate = frame->sample_rate;
    te->te_samples = 0;
  }

  avcodec_close(ctx);
  av_freep(&ctx);

  av_frame_free(&frame);
}


/**
 *
 */
static void
enqueue_packet(ts_demuxer_t *td, const void *data, int len,
               int64_t dts, int64_t pts, int keyframe,
               hls_segment_t *hs, int seq, ts_es_t *te)
{
  int64_t ts_offset;

  dts = rescale(dts);
  pts = rescale(pts);

  if(hs == NULL) {
    ts_offset = te->te_ts_offset;
  } else {

    if(unlikely(hs->hs_ts_offset == PTS_UNSET)) {

      if(!hs->hs_discontinuity) {
        // Not a discontinuity, just steal offset from previous segment
        hls_segment_t *prev = TAILQ_PREV(hs, hls_segment_queue, hs_link);
        if(prev != NULL)
          hs->hs_ts_offset = prev->hs_ts_offset;
      }

      if(hs->hs_ts_offset == PTS_UNSET && dts != PTS_UNSET) {

        hs->hs_ts_offset = dts - hs->hs_time_offset;
#if 0
        printf("TS offset for seq %d: %"PRId64" - %"PRId64" = %"PRId64"\n",
               hs->hs_seq, dts, hs->hs_time_offset, hs->hs_ts_offset);
#endif
        hls_segment_t *next;

        for(next = TAILQ_NEXT(hs, hs_link) ;
            next != NULL && !next->hs_discontinuity ;
            next = TAILQ_NEXT(next, hs_link)) {
          next->hs_ts_offset = hs->hs_ts_offset;
        }
      }

      if(hs->hs_ts_offset == PTS_UNSET) {
        return;
      }
    }
    ts_offset = te->te_ts_offset = hs->hs_ts_offset;
  }

  media_buf_t *mb = media_buf_alloc_unlocked(td->td_mp, len);
  memcpy(mb->mb_data, data, len);
  mb->mb_dts = dts == PTS_UNSET ? PTS_UNSET : dts - ts_offset;
  mb->mb_pts = pts == PTS_UNSET ? PTS_UNSET : pts - ts_offset;
  mb->mb_cw = media_codec_ref(te->te_codec);
  mb->mb_data_type = te->te_data_type;
  mb->mb_keyframe = keyframe;
  mb->mb_sequence = seq;
  mb->mb_epoch = td->td_mp->mp_epoch;
  mb->mb_drive_clock = te->te_data_type == MB_VIDEO;

  if(mb->mb_keyframe && !te->te_logged_keyframe) {
    te->te_logged_keyframe = 1;
    TRACE(TRACE_DEBUG, "HLS", "%s        keyframe %20lld:%20lld\n",
          te->te_data_type == MB_VIDEO ? "VIDEO" : "AUDIO",
          te->te_codec->parser_ctx->dts,
          te->te_codec->parser_ctx->pts);
  }

  mb->mb_stream = te->te_stream;
  TAILQ_INSERT_TAIL(&td->td_packets, mb, mb_link);
}


/**
 *
 */
static void
parse_data(ts_demuxer_t *td, hls_variant_t *hv, ts_es_t *te,
           const uint8_t *data, int size)
{
  media_pipe_t *mp = td->td_mp;
  media_codec_t *mc = te->te_codec;

  if(mc == NULL)
    return;

  while(size > 0) {

    uint8_t *outbuf;
    int outlen;
    int rlen;

    rlen = av_parser_parse2(mc->parser_ctx, mc->fmt_ctx, &outbuf, &outlen,
                            data, size, te->te_pts, te->te_dts,
                            te->te_current_seq);

    if(outlen) {

      int64_t dts        = mc->parser_ctx->dts;
      int64_t pts        = mc->parser_ctx->pts;
      int     seq        = mc->parser_ctx->pos;
      const int keyframe = mc->parser_ctx->key_frame;

      hls_segment_t *hs =
        hv->hv_frozen ? hv_find_segment_by_seq(hv, seq) : NULL;


      media_queue_t *mq;

      if(te->te_data_type == MB_VIDEO) {
        mq = &mp->mp_video;
      } else {
        mq = &mp->mp_audio;
      }

      if(!(mq->mq_demuxer_flags & HLS_QUEUE_KEYFRAME_SEEN)) {
        if(dts == PTS_UNSET)
          goto skip; // We really need a keyframe with a DTS set
        if(!keyframe)
          goto skip;
      }

      if(te->te_probe_frame) {
        // Need to find actual duration be decoding a frame
        probe_duration(te, outbuf, outlen);
      }

      if(te->te_sample_rate != 0) {
        te->te_samples += te->te_samples_per_frame;
        AVRational timebase = {1, te->te_sample_rate};
        int64_t ts = av_rescale_q(te->te_samples, timebase, mpeg_tc);
        dts = pts = te->te_bts + ts;
      }

      enqueue_packet(td, outbuf, outlen, dts, pts, keyframe, hs, seq, te);
    }
  skip:

    te->te_pts = PTS_UNSET;
    te->te_dts = PTS_UNSET;
    data += rlen;
    size -= rlen;
  }
}


static int
h264_check_keyframe(const uint8_t *p, int len)
{
  return (p[0] & 0x1f) == 0x5;
}

static int
is_h264_keyframe(const uint8_t *d, int len)
{
  const uint8_t *p = NULL;
  int is_keyframe = 0;

  len = MIN(len, 1024);

  while(len > 3 && !is_keyframe) {
    if(!(d[0] == 0 && d[1] == 0 && d[2] == 1)) {
      d++;
      len--;
      continue;
    }

    if(p != NULL) {
      is_keyframe |= h264_check_keyframe(p, d - p);
    }

    d += 3;
    len -= 3;
    p = d;
  }
  d += len;

  if(p != NULL)
    is_keyframe |= h264_check_keyframe(p, d - p);

  return is_keyframe;
}



/**
 *
 */
static void
parse_h264(ts_demuxer_t *td, ts_es_t *te, const uint8_t *data, int size,
           hls_segment_t *hs)
{
  int keyframe = is_h264_keyframe(data, size);

  enqueue_packet(td, data, size, te->te_dts, te->te_pts, keyframe, hs,
                 hs->hs_seq, te);
}

/**
 *
 */
static void
emit_packet(ts_es_t *te, ts_demuxer_t *td, hls_segment_t *hs)
{
  const uint8_t *data = te->te_buf;
  int            size = te->te_packet_size;

  if(size < 9)
    return;

  data += 6;
  size -= 6;

  int hlen = parse_pes_header(te, data, size);
  if(hlen < 0)
    return;

  if(!te->te_logged_ts && te->te_pts != PTS_UNSET) {
    te->te_logged_ts = 1;
    TRACE(TRACE_DEBUG, "HLS", "%s First timestamp %20lld:%20lld\n",
          te->te_data_type == MB_VIDEO ? "VIDEO" : "AUDIO",
          te->te_dts, te->te_pts);
  }

  data += hlen;
  size -= hlen;


  if(te->te_data_type == MB_VIDEO && 0)
    parse_h264(td, te, data, size, hs);
  else
    parse_data(td, hs->hs_variant, te, data, size);
}


/**
 *
 */
static void
process_es(ts_es_t *te, const uint8_t *tsb, ts_demuxer_t *td, hls_segment_t *hs)
{
  int off             = tsb[3] & 0x20 ? tsb[4] + 5 : 4;
  int pusi            = tsb[1] & 0x40;
  const uint8_t *data = tsb + off;
  int size            = 188 - off;


  if(pusi) {
    if(te->te_buf != NULL)
      memset(te->te_buf + te->te_packet_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    emit_packet(te, td, hs);
    te->te_packet_size = 0;
    te->te_current_seq = hs->hs_seq;
  }

  if(te->te_packet_size + size > te->te_buf_size) {
    te->te_buf_size = te->te_buf_size * 2 + size;
    te->te_buf = myreallocf(te->te_buf,
                            te->te_buf_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if(te->te_buf == NULL) {
      te->te_buf_size = 0;
      return;
    }
  }

  memcpy(te->te_buf + te->te_packet_size, data, size);
  te->te_packet_size += size;
}


/**
 *
 */
static void
process_tsb(ts_demuxer_t *td, const uint8_t *tsb, hls_segment_t *hs)
{
  ts_service_t *tss;
  ts_es_t *te;
  if(tsb[0] != 0x47)
    return;

  const unsigned int pid = (tsb[1] & 0x1f) << 8 | tsb[2];

  LIST_FOREACH(te, &td->td_elemtary_streams, te_link) {
    if(pid == te->te_pid) {
      process_es(te, tsb, td, hs);
      return;
    }
  }


  LIST_FOREACH(tss, &td->td_services, tss_link) {
    if(pid == tss->tss_pmtpid) {
      parse_table(&tss->tss_pmt, tsb, handle_pmt, tss);
      return;
    }
  }

  if(pid == 0) {
    parse_table(&td->td_pat, tsb, handle_pat, td);
    return;
  }
}



/**
 *
 */
static media_buf_t *
get_pkt(ts_demuxer_t *td)
{
  media_buf_t *mb;

  mb = TAILQ_FIRST(&td->td_packets);
  if(mb != NULL)
    TAILQ_REMOVE(&td->td_packets, mb, mb_link);
  return mb;
}


/**
 *
 */
static void
hls_ts_demuxer_close(hls_variant_t *hv)
{
  ts_demuxer_t *td = hv->hv_demuxer_private;
  assert(td != NULL);

  ts_demuxer_destroy(td);
  hv->hv_demuxer_private = NULL;
  hv->hv_demuxer_close = NULL;
}


/**
 *
 */
static void
hls_ts_demuxer_flush(hls_variant_t *hv)
{
  ts_demuxer_t *td = hv->hv_demuxer_private;
  ts_es_t *te;

  LIST_FOREACH(te, &td->td_elemtary_streams, te_link) {
    media_codec_t *mc = te->te_codec;
    if(mc == NULL)
      continue;

    av_parser_close(mc->parser_ctx);
    mc->parser_ctx = av_parser_init(mc->codec_id);
  }
}



/**
 *
 */
static void
unmuxed_input(ts_demuxer_t *td, const uint8_t *buf, int len, hls_segment_t *hs)
{
  ts_es_t *te = LIST_FIRST(&td->td_elemtary_streams);
  assert(te != NULL);

  te->te_current_seq = hs->hs_seq;

  parse_data(td, hs->hs_variant, te, buf, len);
}


/**
 *
 */
static int
probe_non_muxed(ts_demuxer_t *td, const uint8_t *data, int size,
                hls_segment_t *hs)
{
  int64_t ptsoffset = 0;
  int o = 0;
  hls_variant_t *hv = hs->hs_variant;

  // Not really an ID3 parser, we just search more or less blindly
  if(!memcmp(data, "ID3", 3)) {
    const char *s = "com.apple.streaming.transportStreamTimestamp";
    const uint8_t *t = (const uint8_t *)find_str((const char *)data, size, s);
    if(t != NULL) {
      int off = t - data + 1;
      if(off <= size - 8) {
        ptsoffset = rd64_be(t + strlen(s) + 1);
        o = off + strlen(s) + 8;
      }
    }
  }

  if(o + 7 >= 188)
    goto bad;

  uint32_t h1 = rd32_be(data + o);

  if((h1 & 0xfff60000) == 0xfff00000) {
    // AAC
    ts_es_t *te = find_es(td, 0, 1);

    if(te->te_codec != NULL && te->te_codec->codec_id != AV_CODEC_ID_AAC) {
      media_codec_deref(te->te_codec);
      te->te_codec = NULL;
    }

    if(te->te_codec == NULL) {
      te->te_codec = media_codec_create(AV_CODEC_ID_AAC, 1, NULL, NULL, NULL,
                                        td->td_mp);
      assert(hv->hv_audio_stream != 0);
      te->te_stream = hv->hv_audio_stream;

    }

    te->te_data_type = MB_AUDIO;
    te->te_bts = te->te_dts = te->te_pts = ptsoffset;
    te->te_probe_frame = 1;
    unmuxed_input(td, data + o, size - o, hs);
    return 0;
  }

 bad:
  TRACE(TRACE_ERROR, "HLS", "Unable to probe contents, dumping buffer");
  hexdump("BUFFER", data, MIN(size, 256));
  return -1;
}


/**
 *
 */
static const char *hlserrstr[] = {
  [HLS_ERROR_SEGMENT_NOT_FOUND]     = "Segment not found",
  [HLS_ERROR_SEGMENT_BROKEN]        = "Unable to open segment",
  [HLS_ERROR_SEGMENT_BAD_KEY]       = "Unable to get encryption key",
  [HLS_ERROR_VARIANT_PROBE_ERROR]   = "Probing error",
  [HLS_ERROR_VARIANT_NO_VIDEO]      = "No video",
  [HLS_ERROR_VARIANT_UNKNOWN_AUDIO] = "Unknown audio codec",
};


/**
 *
 */
static void
bad_variant(hls_variant_t *hv, hls_error_t err)
{
  hls_demuxer_t *hd = hv->hv_demuxer;
  hls_t *h = hd->hd_hls;

  HLS_TRACE(h, "Unable to demux variant %s -- %s",
            hv->hv_name, hlserrstr[err]);
  hv->hv_corrupt_counter++;
  hls_variant_close(hv);

  hd->hd_req = hls_demuxer_select_variant(hd);
  hd->hd_last_switch = time(NULL);
}


/**
 *
 */
media_buf_t *
hls_ts_demuxer_read(hls_demuxer_t *hd)
{
  const hls_t *h = hd->hd_hls;
  media_pipe_t *mp = h->h_mp;

  if(hd->hd_current == NULL)
    return HLS_EOF;

  while(1) {

    assert(hd->hd_current != NULL);

    time_t now = time(NULL); // Bad

    hls_check_bw_switch(hd, now);

    if(hd->hd_req != NULL) {

      HLS_TRACE(h, "Switching from %s to %s",
                hd->hd_current->hv_name, hd->hd_req->hv_name);
      hls_variant_close(hd->hd_current);

      hd->hd_current = hd->hd_req;
      hd->hd_req = NULL;

      if(hd == &h->h_primary) {
        // Primary demuxer always control the video queue
        mp->mp_video.mq_demuxer_flags |= HLS_QUEUE_MERGE;
        mp->mp_video.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;

        if(h->h_audio.hd_current == NULL) {
          // If no audio variant is running we are also controlling
          // the audio queue
          mp->mp_audio.mq_demuxer_flags |= HLS_QUEUE_MERGE;
          mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
        }
      }

      if(hd == &h->h_audio) {
        // If the audio demuxer is switching we by definition
        // control the audio queue
        mp->mp_audio.mq_demuxer_flags |= HLS_QUEUE_MERGE;
        mp->mp_audio.mq_demuxer_flags &= ~HLS_QUEUE_KEYFRAME_SEEN;
      }
    }


    hls_variant_t *hv = hd->hd_current;
    ts_demuxer_t *td = hv->hv_demuxer_private;

    if(td == NULL) {
      assert(hv->hv_demuxer_private == NULL);
      td = hv->hv_demuxer_private = calloc(1, sizeof(ts_demuxer_t));
      TAILQ_INIT(&td->td_packets);
      assert(hv->hv_demuxer_close == NULL);
      hv->hv_demuxer_close = hls_ts_demuxer_close;
      hv->hv_demuxer_flush = hls_ts_demuxer_flush;
      td->td_mp = mp;
      td->td_hd = hd;
    }

    media_buf_t *mb = get_pkt(td);
    if(mb != NULL)
      return mb;

    if(hd->hd_seek_to_segment != PTS_UNSET && hv->hv_current_seg != NULL) {
      hls_segment_close(hv->hv_current_seg);
      hv->hv_current_seg = NULL;
    }

    if(hv->hv_current_seg == NULL || hv->hv_current_seg->hs_fh == NULL) {

      hls_segment_t *hs;
      while(1) {

        now = time(NULL);

        hs = hls_variant_select_next_segment(hv, now);

        if(hs == HLS_NYA)
          continue;

        if(hs == HLS_EOF)
          return HLS_EOF;

        if(hs == NULL)
          return NULL;

        hls_variant_open(hv);

        HLS_TRACE(h, "%s: Opening variant %s sequence %d discontinuity:%s",
                  hd->hd_type, hv->hv_name, hs->hs_seq,
                  hs->hs_discontinuity ? "Yes" : "No");

        break;
      }

      int attempts = 0;

      while(1) {

        assert(hs != NULL);
        hls_error_t err = hls_segment_open(hs);

        if(h->h_exit_event != NULL || hd->hd_seek_to_segment != PTS_UNSET) {
          hls_segment_close(hs);
          hv->hv_current_seg = NULL;
          return NULL;
        }

        if(!hv->hv_frozen && err == HLS_ERROR_SEGMENT_NOT_FOUND) {
          /*
           * When in live mode segments may disappear and with a bit
           * of unluck that can happen between when we load the
           * playlist and try to open the segment, so retry a few times
           */
          if(attempts < 5) {
            attempts++;
            hs = TAILQ_NEXT(hs, hs_link);
            if(hs != NULL) {
              HLS_TRACE(h, "Segment %d not found in live mode, trying next",
                        hs->hs_seq - 1);
              continue;
            }
          }
        }

        if(err) {
          bad_variant(hv, err);
          return NULL;
        }
        hv->hv_current_seg = hs;
        td->td_mux_mode = TD_MUX_MODE_UNSET;
        break;
      }
    }

    // Ok, try to read some bytes

    hls_segment_t *hs = hv->hv_current_seg;
    int r;

    switch(td->td_mux_mode) {

    case TD_MUX_MODE_UNSET:
      HLS_TRACE(h, "Probing variant %s, sequence %d",
                hv->hv_name, hs->hs_seq);

      while(td->td_buf_bytes < sizeof(td->td_buf)) {
        r = fa_read(hs->hs_fh,
                    td->td_buf + td->td_buf_bytes,
                    sizeof(td->td_buf) - td->td_buf_bytes);
        if(r <= 0) {
          bad_variant(hv, HLS_ERROR_VARIANT_PROBE_ERROR);
          return NULL;
        }

        td->td_buf_bytes += r;
      }

      // Search stream for TS mux lock, we want two continous packets

      int i;
      for(i = 0; i < sizeof(td->td_buf) - 188 * 2; i++)
        if(td->td_buf[i] == 0x47 &&
           td->td_buf[i + 188] == 0x47 &&
           td->td_buf[i + 188 * 2] == 0x47)
          break;

      if(i != sizeof(td->td_buf) - 188 * 2) {

        td->td_mux_mode = TD_MUX_MODE_TS;

        while(i <= sizeof(td->td_buf) - 188) {
          process_tsb(td, td->td_buf + i, hs);
          i += 188;
        }

        int spill = sizeof(td->td_buf) - i;
        assert(spill < 188);

        td->td_buf_bytes = spill;
        memmove(td->td_buf, td->td_buf + i, spill);
        break;
      }


      if(hd == &h->h_primary) {
        bad_variant(hv, HLS_ERROR_VARIANT_NO_VIDEO);
        return NULL;
      }

      if(probe_non_muxed(td, td->td_buf, sizeof(td->td_buf), hs)) {
        bad_variant(hv, HLS_ERROR_VARIANT_UNKNOWN_AUDIO);
        return NULL;
      }

      td->td_mux_mode = TD_MUX_MODE_RAW;
      td->td_buf_bytes = 0;
      break;

    case TD_MUX_MODE_RAW:
      r = fa_read(hs->hs_fh, td->td_buf, sizeof(td->td_buf));

      if(r < 0)
        return HLS_EOF;

      if(r == 0) {
        hls_segment_close(hs);
        continue;
      }

      unmuxed_input(td, td->td_buf, r, hs);
      break;

    case TD_MUX_MODE_TS:

      assert(td->td_buf_bytes < 188);
      r = fa_read(hs->hs_fh,
                  td->td_buf + td->td_buf_bytes,
                  188 - td->td_buf_bytes);

      if(r < 0)
        return HLS_EOF;

      if(r == 0) {
        hls_segment_close(hs);
        continue;
      }

      td->td_buf_bytes += r;

      if(td->td_buf_bytes == 188) {
        td->td_buf_bytes = 0;
        process_tsb(td, td->td_buf, hs);
      }
      break;
    }
  }

  return NULL;
}
