#include <unistd.h>

#include "showtime.h"
#include "audio2/audio.h"
#include "media/media.h"


typedef struct decoder {
  audio_decoder_t ad;
} decoder_t;


/**
 *
 */
static void
dummy_audio_fini(audio_decoder_t *ad)
{
}


/**
 *
 */
static int
dummy_audio_reconfig(audio_decoder_t *ad)
{

  dummy_audio_fini(ad);

  ad->ad_out_sample_format = AV_SAMPLE_FMT_S16;
  ad->ad_out_sample_rate = 48000;
  ad->ad_out_channel_layout = AV_CH_LAYOUT_STEREO;
  ad->ad_tile_size = 1024;

  return 0;
}


/**
 *
 */
static int
dummy_audio_deliver(audio_decoder_t *ad, int samples, int64_t pts, int epoch)
{
  int sleeptime = 1000000LL * samples / 48000;
  usleep(sleeptime);
  return 0;
}


/**
 *
 */
static void
dummy_audio_pause(audio_decoder_t *ad)
{
}


/**
 *
 */
static void
dummy_audio_play(audio_decoder_t *ad)
{
}


/**
 *
 */
static void
dummy_audio_flush(audio_decoder_t *ad)
{
}


/**
 *
 */
static audio_class_t dummy_audio_class = {
  .ac_alloc_size       = sizeof(decoder_t),
  .ac_fini             = dummy_audio_fini,
  .ac_reconfig         = dummy_audio_reconfig,
  .ac_deliver_unlocked = dummy_audio_deliver,
  .ac_pause            = dummy_audio_pause,
  .ac_play             = dummy_audio_play,
  .ac_flush            = dummy_audio_flush,
};



/**
 *
 */
audio_class_t *
audio_driver_init(struct prop *asettings, struct htsmsg *store)
{
  return &dummy_audio_class;
}

