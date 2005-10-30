/*
 * QEMU Audio subsystem
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "vl.h"

#define AUDIO_CAP "audio"
#include "audio_int.h"

static void audio_pcm_hw_fini_in (HWVoiceIn *hw);
static void audio_pcm_hw_fini_out (HWVoiceOut *hw);

static LIST_HEAD (hw_in_listhead, HWVoiceIn) hw_head_in;
static LIST_HEAD (hw_out_listhead, HWVoiceOut) hw_head_out;

/* #define DEBUG_PLIVE */
/* #define DEBUG_LIVE */
/* #define DEBUG_OUT */

static struct audio_driver *drvtab[] = {
#ifdef CONFIG_OSS
    &oss_audio_driver,
#endif
#ifdef CONFIG_ALSA
    &alsa_audio_driver,
#endif
#ifdef CONFIG_COREAUDIO
    &coreaudio_audio_driver,
#endif
#ifdef CONFIG_DSOUND
    &dsound_audio_driver,
#endif
#ifdef CONFIG_FMOD
    &fmod_audio_driver,
#endif
#ifdef CONFIG_SDL
    &sdl_audio_driver,
#endif
    &no_audio_driver,
    &wav_audio_driver
};

AudioState audio_state = {
    /* Out */
    1,                          /* use fixed settings */
    44100,                      /* fixed frequency */
    2,                          /* fixed channels */
    AUD_FMT_S16,                /* fixed format */
    1,                          /* number of hw voices */
    1,                          /* greedy */

    /* In */
    1,                          /* use fixed settings */
    44100,                      /* fixed frequency */
    2,                          /* fixed channels */
    AUD_FMT_S16,                /* fixed format */
    1,                          /* number of hw voices */
    1,                          /* greedy */

    NULL,                       /* driver opaque */
    NULL,                       /* driver */

    NULL,                       /* timer handle */
    { 0 },                      /* period */
    0                           /* plive */
};

volume_t nominal_volume = {
    0,
#ifdef FLOAT_MIXENG
    1.0,
    1.0
#else
    UINT_MAX,
    UINT_MAX
#endif
};

/* http://www.df.lth.se/~john_e/gems/gem002d.html */
/* http://www.multi-platforms.com/Tips/PopCount.htm */
uint32_t popcount (uint32_t u)
{
    u = ((u&0x55555555) + ((u>>1)&0x55555555));
    u = ((u&0x33333333) + ((u>>2)&0x33333333));
    u = ((u&0x0f0f0f0f) + ((u>>4)&0x0f0f0f0f));
    u = ((u&0x00ff00ff) + ((u>>8)&0x00ff00ff));
    u = ( u&0x0000ffff) + (u>>16);
    return u;
}

inline uint32_t lsbindex (uint32_t u)
{
    return popcount ((u&-u)-1);
}

#ifdef AUDIO_IS_FLAWLESS_AND_NO_CHECKS_ARE_REQURIED
#error No its not
#else
int audio_bug (const char *funcname, int cond)
{
    if (cond) {
        static int shown;

        AUD_log (NULL, "Error a bug that was just triggered in %s\n", funcname);
        if (!shown) {
            shown = 1;
            AUD_log (NULL, "Save all your work and restart without audio\n");
            AUD_log (NULL, "Please send bug report to malc@pulsesoft.com\n");
            AUD_log (NULL, "I am sorry\n");
        }
        AUD_log (NULL, "Context:\n");

#if defined AUDIO_BREAKPOINT_ON_BUG
#  if defined HOST_I386
#    if defined __GNUC__
        __asm__ ("int3");
#    elif defined _MSC_VER
        _asm _emit 0xcc;
#    else
        abort ();
#    endif
#  else
        abort ();
#  endif
#endif
    }

    return cond;
}
#endif

static char *audio_alloc_prefix (const char *s)
{
    const char qemu_prefix[] = "QEMU_";
    size_t len;
    char *r;

    if (!s) {
        return NULL;
    }

    len = strlen (s);
    r = qemu_malloc (len + sizeof (qemu_prefix));

    if (r) {
        size_t i;
        char *u = r + sizeof (qemu_prefix) - 1;

        strcpy (r, qemu_prefix);
        strcat (r, s);

        for (i = 0; i < len; ++i) {
            u[i] = toupper (u[i]);
        }
    }
    return r;
}

const char *audio_audfmt_to_string (audfmt_e fmt)
{
    switch (fmt) {
    case AUD_FMT_U8:
        return "U8";

    case AUD_FMT_U16:
        return "U16";

    case AUD_FMT_S8:
        return "S8";

    case AUD_FMT_S16:
        return "S16";
    }

    dolog ("Bogus audfmt %d returning S16\n", fmt);
    return "S16";
}

audfmt_e audio_string_to_audfmt (const char *s, audfmt_e defval, int *defaultp)
{
    if (!strcasecmp (s, "u8")) {
        *defaultp = 0;
        return AUD_FMT_U8;
    }
    else if (!strcasecmp (s, "u16")) {
        *defaultp = 0;
        return AUD_FMT_U16;
    }
    else if (!strcasecmp (s, "s8")) {
        *defaultp = 0;
        return AUD_FMT_S8;
    }
    else if (!strcasecmp (s, "s16")) {
        *defaultp = 0;
        return AUD_FMT_S16;
    }
    else {
        dolog ("Bogus audio format `%s' using %s\n",
               s, audio_audfmt_to_string (defval));
        *defaultp = 1;
        return defval;
    }
}

static audfmt_e audio_get_conf_fmt (const char *envname,
                                    audfmt_e defval,
                                    int *defaultp)
{
    const char *var = getenv (envname);
    if (!var) {
        *defaultp = 1;
        return defval;
    }
    return audio_string_to_audfmt (var, defval, defaultp);
}

static int audio_get_conf_int (const char *key, int defval, int *defaultp)
{
    int val;
    char *strval;

    strval = getenv (key);
    if (strval) {
        *defaultp = 0;
        val = atoi (strval);
        return val;
    }
    else {
        *defaultp = 1;
        return defval;
    }
}

static const char *audio_get_conf_str (const char *key,
                                       const char *defval,
                                       int *defaultp)
{
    const char *val = getenv (key);
    if (!val) {
        *defaultp = 1;
        return defval;
    }
    else {
        *defaultp = 0;
        return val;
    }
}

void AUD_log (const char *cap, const char *fmt, ...)
{
    va_list ap;
    if (cap) {
        fprintf (stderr, "%s: ", cap);
    }
    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
}

void AUD_vlog (const char *cap, const char *fmt, va_list ap)
{
    if (cap) {
        fprintf (stderr, "%s: ", cap);
    }
    vfprintf (stderr, fmt, ap);
}

static void audio_print_options (const char *prefix,
                                 struct audio_option *opt)
{
    char *uprefix;

    if (!prefix) {
        dolog ("No prefix specified\n");
        return;
    }

    if (!opt) {
        dolog ("No options\n");
        return;
    }

    uprefix = audio_alloc_prefix (prefix);

    for (; opt->name; opt++) {
        const char *state = "default";
        printf ("  %s_%s: ", uprefix, opt->name);

        if (opt->overridenp && *opt->overridenp) {
            state = "current";
        }

        switch (opt->tag) {
        case AUD_OPT_BOOL:
            {
                int *intp = opt->valp;
                printf ("boolean, %s = %d\n", state, *intp ? 1 : 0);
            }
            break;

        case AUD_OPT_INT:
            {
                int *intp = opt->valp;
                printf ("integer, %s = %d\n", state, *intp);
            }
            break;

        case AUD_OPT_FMT:
            {
                audfmt_e *fmtp = opt->valp;
                printf (
                    "format, %s = %s, (one of: U8 S8 U16 S16)\n",
                    state,
                    audio_audfmt_to_string (*fmtp)
                    );
            }
            break;

        case AUD_OPT_STR:
            {
                const char **strp = opt->valp;
                printf ("string, %s = %s\n",
                        state,
                        *strp ? *strp : "(not set)");
            }
            break;

        default:
            printf ("???\n");
            dolog ("Bad value tag for option %s_%s %d\n",
                   uprefix, opt->name, opt->tag);
            break;
        }
        printf ("    %s\n", opt->descr);
    }

    qemu_free (uprefix);
}

static void audio_process_options (const char *prefix,
                                   struct audio_option *opt)
{
    char *optname;
    const char qemu_prefix[] = "QEMU_";
    size_t preflen;

    if (audio_bug (AUDIO_FUNC, !prefix)) {
        dolog ("prefix = NULL\n");
        return;
    }

    if (audio_bug (AUDIO_FUNC, !opt)) {
        dolog ("opt = NULL\n");
        return;
    }

    preflen = strlen (prefix);

    for (; opt->name; opt++) {
        size_t len, i;
        int def;

        if (!opt->valp) {
            dolog ("Option value pointer for `%s' is not set\n",
                   opt->name);
            continue;
        }

        len = strlen (opt->name);
        optname = qemu_malloc (len + preflen + sizeof (qemu_prefix) + 1);
        if (!optname) {
            dolog ("Can not allocate memory for option name `%s'\n",
                   opt->name);
            continue;
        }

        strcpy (optname, qemu_prefix);
        for (i = 0; i <= preflen; ++i) {
            optname[i + sizeof (qemu_prefix) - 1] = toupper (prefix[i]);
        }
        strcat (optname, "_");
        strcat (optname, opt->name);

        def = 1;
        switch (opt->tag) {
        case AUD_OPT_BOOL:
        case AUD_OPT_INT:
            {
                int *intp = opt->valp;
                *intp = audio_get_conf_int (optname, *intp, &def);
            }
            break;

        case AUD_OPT_FMT:
            {
                audfmt_e *fmtp = opt->valp;
                *fmtp = audio_get_conf_fmt (optname, *fmtp, &def);
            }
            break;

        case AUD_OPT_STR:
            {
                const char **strp = opt->valp;
                *strp = audio_get_conf_str (optname, *strp, &def);
            }
            break;

        default:
            dolog ("Bad value tag for option `%s' - %d\n",
                   optname, opt->tag);
            break;
        }

        if (!opt->overridenp) {
            opt->overridenp = &opt->overriden;
        }
        *opt->overridenp = !def;
        qemu_free (optname);
    }
}

static int audio_pcm_info_eq (struct audio_pcm_info *info, int freq,
                              int nchannels, audfmt_e fmt)
{
    int bits = 8, sign = 0;

    switch (fmt) {
    case AUD_FMT_S8:
        sign = 1;
    case AUD_FMT_U8:
        break;

    case AUD_FMT_S16:
        sign = 1;
    case AUD_FMT_U16:
        bits = 16;
        break;
    }
    return info->freq == freq
        && info->nchannels == nchannels
        && info->sign == sign
        && info->bits == bits;
}

void audio_pcm_init_info (struct audio_pcm_info *info, int freq,
                          int nchannels, audfmt_e fmt, int swap_endian)
{
    int bits = 8, sign = 0;

    switch (fmt) {
    case AUD_FMT_S8:
        sign = 1;
    case AUD_FMT_U8:
        break;

    case AUD_FMT_S16:
        sign = 1;
    case AUD_FMT_U16:
        bits = 16;
        break;
    }

    info->freq = freq;
    info->bits = bits;
    info->sign = sign;
    info->nchannels = nchannels;
    info->shift = (nchannels == 2) + (bits == 16);
    info->align = (1 << info->shift) - 1;
    info->bytes_per_second = info->freq << info->shift;
    info->swap_endian = swap_endian;
}

void audio_pcm_info_clear_buf (struct audio_pcm_info *info, void *buf, int len)
{
    if (!len) {
        return;
    }

    if (info->sign) {
        memset (buf, len << info->shift, 0x00);
    }
    else {
        if (info->bits == 8) {
            memset (buf, len << info->shift, 0x80);
        }
        else {
            int i;
            uint16_t *p = buf;
            int shift = info->nchannels - 1;
            short s = INT16_MAX;

            if (info->swap_endian) {
                s = bswap16 (s);
            }

            for (i = 0; i < len << shift; i++) {
                p[i] = s;
            }
        }
    }
}

/*
 * Hard voice (capture)
 */
static void audio_pcm_hw_free_resources_in (HWVoiceIn *hw)
{
    if (hw->conv_buf) {
        qemu_free (hw->conv_buf);
    }
    hw->conv_buf = NULL;
}

static int audio_pcm_hw_alloc_resources_in (HWVoiceIn *hw)
{
    hw->conv_buf = qemu_mallocz (hw->samples * sizeof (st_sample_t));
    if (!hw->conv_buf) {
        return -1;
    }
    return 0;
}

static int audio_pcm_hw_init_in (HWVoiceIn *hw, int freq, int nchannels, audfmt_e fmt)
{
    audio_pcm_hw_fini_in (hw);

    if (hw->pcm_ops->init_in (hw, freq, nchannels, fmt)) {
        memset (hw, 0, audio_state.drv->voice_size_in);
        return -1;
    }
    LIST_INIT (&hw->sw_head);
    hw->active = 1;
    hw->samples = hw->bufsize >> hw->info.shift;
    hw->conv =
        mixeng_conv
        [nchannels == 2]
        [hw->info.sign]
        [hw->info.swap_endian]
        [hw->info.bits == 16];
    if (audio_pcm_hw_alloc_resources_in (hw)) {
        audio_pcm_hw_free_resources_in (hw);
        return -1;
    }
    return 0;
}

static uint64_t audio_pcm_hw_find_min_in (HWVoiceIn *hw)
{
    SWVoiceIn *sw;
    int m = hw->total_samples_captured;

    for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        if (sw->active) {
            m = audio_MIN (m, sw->total_hw_samples_acquired);
        }
    }
    return m;
}

int audio_pcm_hw_get_live_in (HWVoiceIn *hw)
{
    int live = hw->total_samples_captured - audio_pcm_hw_find_min_in (hw);
    if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
        dolog ("live=%d hw->samples=%d\n", live, hw->samples);
        return 0;
    }
    return live;
}

/*
 * Soft voice (capture)
 */
static void audio_pcm_sw_free_resources_in (SWVoiceIn *sw)
{
    if (sw->conv_buf) {
        qemu_free (sw->conv_buf);
    }

    if (sw->rate) {
        st_rate_stop (sw->rate);
    }

    sw->conv_buf = NULL;
    sw->rate = NULL;
}

static int audio_pcm_sw_alloc_resources_in (SWVoiceIn *sw)
{
    int samples = ((int64_t) sw->hw->samples << 32) / sw->ratio;
    sw->conv_buf = qemu_mallocz (samples * sizeof (st_sample_t));
    if (!sw->conv_buf) {
        return -1;
    }

    sw->rate = st_rate_start (sw->hw->info.freq, sw->info.freq);
    if (!sw->rate) {
        qemu_free (sw->conv_buf);
        sw->conv_buf = NULL;
        return -1;
    }
    return 0;
}

static int audio_pcm_sw_init_in (SWVoiceIn *sw, HWVoiceIn *hw, const char *name,
                           int freq, int nchannels, audfmt_e fmt)
{
    audio_pcm_init_info (&sw->info, freq, nchannels, fmt,
                         /* None of the cards emulated by QEMU are big-endian
                            hence following shortcut */
                         audio_need_to_swap_endian (0));
    sw->hw = hw;
    sw->ratio = ((int64_t) sw->info.freq << 32) / sw->hw->info.freq;

    sw->clip =
        mixeng_clip
        [nchannels == 2]
        [sw->info.sign]
        [sw->info.swap_endian]
        [sw->info.bits == 16];

    sw->name = qemu_strdup (name);
    audio_pcm_sw_free_resources_in (sw);
    return audio_pcm_sw_alloc_resources_in (sw);
}

static int audio_pcm_sw_get_rpos_in (SWVoiceIn *sw)
{
    HWVoiceIn *hw = sw->hw;
    int live = hw->total_samples_captured - sw->total_hw_samples_acquired;
    int rpos;

    if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
        dolog ("live=%d hw->samples=%d\n", live, hw->samples);
        return 0;
    }

    rpos = hw->wpos - live;
    if (rpos >= 0) {
        return rpos;
    }
    else {
        return hw->samples + rpos;
    }
}

int audio_pcm_sw_read (SWVoiceIn *sw, void *buf, int size)
{
    HWVoiceIn *hw = sw->hw;
    int samples, live, ret = 0, swlim, isamp, osamp, rpos, total = 0;
    st_sample_t *src, *dst = sw->conv_buf;

    rpos = audio_pcm_sw_get_rpos_in (sw) % hw->samples;

    live = hw->total_samples_captured - sw->total_hw_samples_acquired;
    if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
        dolog ("live_in=%d hw->samples=%d\n", live, hw->samples);
        return 0;
    }

    samples = size >> sw->info.shift;
    if (!live) {
        return 0;
    }

    swlim = (live * sw->ratio) >> 32;
    swlim = audio_MIN (swlim, samples);

    while (swlim) {
        src = hw->conv_buf + rpos;
        isamp = hw->wpos - rpos;
        /* XXX: <= ? */
        if (isamp <= 0) {
            isamp = hw->samples - rpos;
        }

        if (!isamp) {
            break;
        }
        osamp = swlim;

        if (audio_bug (AUDIO_FUNC, osamp < 0)) {
            dolog ("osamp=%d\n", osamp);
        }

        st_rate_flow (sw->rate, src, dst, &isamp, &osamp);
        swlim -= osamp;
        rpos = (rpos + isamp) % hw->samples;
        dst += osamp;
        ret += osamp;
        total += isamp;
    }

    sw->clip (buf, sw->conv_buf, ret);
    sw->total_hw_samples_acquired += total;
    return ret << sw->info.shift;
}

/*
 * Hard voice (playback)
 */
static int audio_pcm_hw_find_min_out (HWVoiceOut *hw, int *nb_livep)
{
    SWVoiceOut *sw;
    int m = INT_MAX;
    int nb_live = 0;

    for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        if (sw->active || !sw->empty) {
            m = audio_MIN (m, sw->total_hw_samples_mixed);
            nb_live += 1;
        }
    }

    *nb_livep = nb_live;
    return m;
}

static void audio_pcm_hw_free_resources_out (HWVoiceOut *hw)
{
    if (hw->mix_buf) {
        qemu_free (hw->mix_buf);
    }

    hw->mix_buf = NULL;
}

static int audio_pcm_hw_alloc_resources_out (HWVoiceOut *hw)
{
    hw->mix_buf = qemu_mallocz (hw->samples * sizeof (st_sample_t));
    if (!hw->mix_buf) {
        return -1;
    }

    return 0;
}

static int audio_pcm_hw_init_out (HWVoiceOut *hw, int freq,
                            int nchannels, audfmt_e fmt)
{
    audio_pcm_hw_fini_out (hw);
    if (hw->pcm_ops->init_out (hw, freq, nchannels, fmt)) {
        memset (hw, 0, audio_state.drv->voice_size_out);
        return -1;
    }

    LIST_INIT (&hw->sw_head);
    hw->active = 1;
    hw->samples = hw->bufsize >> hw->info.shift;
    hw->clip =
        mixeng_clip
        [nchannels == 2]
        [hw->info.sign]
        [hw->info.swap_endian]
        [hw->info.bits == 16];
    if (audio_pcm_hw_alloc_resources_out (hw)) {
        audio_pcm_hw_fini_out (hw);
        return -1;
    }
    return 0;
}

int audio_pcm_hw_get_live_out2 (HWVoiceOut *hw, int *nb_live)
{
    int smin;

    smin = audio_pcm_hw_find_min_out (hw, nb_live);

    if (!*nb_live) {
        return 0;
    }
    else {
        int live = smin;

        if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
            dolog ("live=%d hw->samples=%d\n", live, hw->samples);
            return 0;
        }
        return live;
    }
}

int audio_pcm_hw_get_live_out (HWVoiceOut *hw)
{
    int nb_live;
    int live;

    live = audio_pcm_hw_get_live_out2 (hw, &nb_live);
    if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
        dolog ("live=%d hw->samples=%d\n", live, hw->samples);
        return 0;
    }
    return live;
}

/*
 * Soft voice (playback)
 */
static void audio_pcm_sw_free_resources_out (SWVoiceOut *sw)
{
    if (sw->buf) {
        qemu_free (sw->buf);
    }

    if (sw->rate) {
        st_rate_stop (sw->rate);
    }

    sw->buf = NULL;
    sw->rate = NULL;
}

static int audio_pcm_sw_alloc_resources_out (SWVoiceOut *sw)
{
    sw->buf = qemu_mallocz (sw->hw->samples * sizeof (st_sample_t));
    if (!sw->buf) {
        return -1;
    }

    sw->rate = st_rate_start (sw->info.freq, sw->hw->info.freq);
    if (!sw->rate) {
        qemu_free (sw->buf);
        sw->buf = NULL;
        return -1;
    }
    return 0;
}

static int audio_pcm_sw_init_out (SWVoiceOut *sw, HWVoiceOut *hw,
                            const char *name, int freq,
                            int nchannels, audfmt_e fmt)
{
    audio_pcm_init_info (&sw->info, freq, nchannels, fmt,
                         /* None of the cards emulated by QEMU are big-endian
                            hence following shortcut */
                         audio_need_to_swap_endian (0));
    sw->hw = hw;
    sw->empty = 1;
    sw->active = 0;
    sw->ratio = ((int64_t) sw->hw->info.freq << 32) / sw->info.freq;
    sw->total_hw_samples_mixed = 0;

    sw->conv =
        mixeng_conv
        [nchannels == 2]
        [sw->info.sign]
        [sw->info.swap_endian]
        [sw->info.bits == 16];
    sw->name = qemu_strdup (name);

    audio_pcm_sw_free_resources_out (sw);
    return audio_pcm_sw_alloc_resources_out (sw);
}

int audio_pcm_sw_write (SWVoiceOut *sw, void *buf, int size)
{
    int hwsamples, samples, isamp, osamp, wpos, live, dead, left, swlim, blck;
    int ret = 0, pos = 0, total = 0;

    if (!sw) {
        return size;
    }

    hwsamples = sw->hw->samples;

    live = sw->total_hw_samples_mixed;
    if (audio_bug (AUDIO_FUNC, live < 0 || live > hwsamples)){
        dolog ("live=%d hw->samples=%d\n", live, hwsamples);
        return 0;
    }

    if (live == hwsamples) {
        return 0;
    }

    wpos = (sw->hw->rpos + live) % hwsamples;
    samples = size >> sw->info.shift;

    dead = hwsamples - live;
    swlim = ((int64_t) dead << 32) / sw->ratio;
    swlim = audio_MIN (swlim, samples);
    if (swlim) {
        sw->conv (sw->buf, buf, swlim, &sw->vol);
    }

    while (swlim) {
        dead = hwsamples - live;
        left = hwsamples - wpos;
        blck = audio_MIN (dead, left);
        if (!blck) {
            break;
        }
        isamp = swlim;
        osamp = blck;
        st_rate_flow_mix (
            sw->rate,
            sw->buf + pos,
            sw->hw->mix_buf + wpos,
            &isamp,
            &osamp
            );
        ret += isamp;
        swlim -= isamp;
        pos += isamp;
        live += osamp;
        wpos = (wpos + osamp) % hwsamples;
        total += osamp;
    }

    sw->total_hw_samples_mixed += total;
    sw->empty = sw->total_hw_samples_mixed == 0;

#ifdef DEBUG_OUT
    dolog (
        "%s: write size %d ret %d total sw %d, hw %d\n",
        sw->name,
        size >> sw->info.shift,
        ret,
        sw->total_hw_samples_mixed,
        sw->hw->total_samples_played
        );
#endif

    return ret << sw->info.shift;
}

#ifdef DEBUG_AUDIO
static void audio_pcm_print_info (const char *cap, struct audio_pcm_info *info)
{
    dolog ("%s: bits %d, sign %d, freq %d, nchan %d\n",
           cap, info->bits, info->sign, info->freq, info->nchannels);
}
#endif

#define DAC
#include "audio_template.h"
#undef DAC
#include "audio_template.h"

int AUD_write (SWVoiceOut *sw, void *buf, int size)
{
    int bytes;

    if (!sw) {
        /* XXX: Consider options */
        return size;
    }

    if (!sw->hw->enabled) {
        dolog ("Writing to disabled voice %s\n", sw->name);
        return 0;
    }

    bytes = sw->hw->pcm_ops->write (sw, buf, size);
    return bytes;
}

int AUD_read (SWVoiceIn *sw, void *buf, int size)
{
    int bytes;

    if (!sw) {
        /* XXX: Consider options */
        return size;
    }

    if (!sw->hw->enabled) {
        dolog ("Reading from disabled voice %s\n", sw->name);
        return 0;
    }

    bytes = sw->hw->pcm_ops->read (sw, buf, size);
    return bytes;
}

int AUD_get_buffer_size_out (SWVoiceOut *sw)
{
    return sw->hw->bufsize;
}

void AUD_set_active_out (SWVoiceOut *sw, int on)
{
    HWVoiceOut *hw;

    if (!sw) {
        return;
    }

    hw = sw->hw;
    if (sw->active != on) {
        SWVoiceOut *temp_sw;

        if (on) {
            int total;

            hw->pending_disable = 0;
            if (!hw->enabled) {
                hw->enabled = 1;
                hw->pcm_ops->ctl_out (hw, VOICE_ENABLE);
            }

            if (sw->empty) {
                total = 0;
            }
        }
        else {
            if (hw->enabled) {
                int nb_active = 0;

                for (temp_sw = hw->sw_head.lh_first; temp_sw;
                     temp_sw = temp_sw->entries.le_next) {
                    nb_active += temp_sw->active != 0;
                }

                hw->pending_disable = nb_active == 1;
            }
        }
        sw->active = on;
    }
}

void AUD_set_active_in (SWVoiceIn *sw, int on)
{
    HWVoiceIn *hw;

    if (!sw) {
        return;
    }

    hw = sw->hw;
    if (sw->active != on) {
        SWVoiceIn *temp_sw;

        if (on) {
            if (!hw->enabled) {
                hw->enabled = 1;
                hw->pcm_ops->ctl_in (hw, VOICE_ENABLE);
            }
            sw->total_hw_samples_acquired = hw->total_samples_captured;
        }
        else {
            if (hw->enabled) {
                int nb_active = 0;

                for (temp_sw = hw->sw_head.lh_first; temp_sw;
                     temp_sw = temp_sw->entries.le_next) {
                    nb_active += temp_sw->active != 0;
                }

                if (nb_active == 1) {
                    hw->enabled = 0;
                    hw->pcm_ops->ctl_in (hw, VOICE_DISABLE);
                }
            }
        }
        sw->active = on;
    }
}

static int audio_get_avail (SWVoiceIn *sw)
{
    int live;

    if (!sw) {
        return 0;
    }

    live = sw->hw->total_samples_captured - sw->total_hw_samples_acquired;
    if (audio_bug (AUDIO_FUNC, live < 0 || live > sw->hw->samples)) {
        dolog ("live=%d sw->hw->samples=%d\n", live, sw->hw->samples);
        return 0;
    }

    ldebug (
        "%s: get_avail live %d ret %lld\n",
        sw->name,
        live, (((int64_t) live << 32) / sw->ratio) << sw->info.shift
        );

    return (((int64_t) live << 32) / sw->ratio) << sw->info.shift;
}

static int audio_get_free (SWVoiceOut *sw)
{
    int live, dead;

    if (!sw) {
        return 0;
    }

    live = sw->total_hw_samples_mixed;

    if (audio_bug (AUDIO_FUNC, live < 0 || live > sw->hw->samples)) {
        dolog ("live=%d sw->hw->samples=%d\n", live, sw->hw->samples);
    }

    dead = sw->hw->samples - live;

#ifdef DEBUG_OUT
    dolog ("%s: get_free live %d dead %d ret %lld\n",
           sw->name,
           live, dead, (((int64_t) dead << 32) / sw->ratio) << sw->info.shift);
#endif

    return (((int64_t) dead << 32) / sw->ratio) << sw->info.shift;
}

static void audio_run_out (void)
{
    HWVoiceOut *hw = NULL;
    SWVoiceOut *sw;

    while ((hw = audio_pcm_hw_find_any_active_enabled_out (hw))) {
        int played;
        int live, free, nb_live;

        live = audio_pcm_hw_get_live_out2 (hw, &nb_live);
        if (!nb_live) {
            live = 0;
        }
        if (audio_bug (AUDIO_FUNC, live < 0 || live > hw->samples)) {
            dolog ("live=%d hw->samples=%d\n", live, hw->samples);
        }

        if (hw->pending_disable && !nb_live) {
#ifdef DEBUG_OUT
            dolog ("Disabling voice\n");
#endif
            hw->enabled = 0;
            hw->pending_disable = 0;
            hw->pcm_ops->ctl_out (hw, VOICE_DISABLE);
            continue;
        }

        if (!live) {
            for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
                if (sw->active) {
                    free = audio_get_free (sw);
                    if (free > 0) {
                        sw->callback.fn (sw->callback.opaque, free);
                    }
                }
            }
            continue;
        }

        played = hw->pcm_ops->run_out (hw);
        if (audio_bug (AUDIO_FUNC, hw->rpos >= hw->samples)) {
            dolog ("hw->rpos=%d hw->samples=%d played=%d\n",
                   hw->rpos, hw->samples, played);
            hw->rpos = 0;
        }

#ifdef DEBUG_OUT
        dolog ("played = %d total %d\n", played, hw->total_samples_played);
#endif

        if (played) {
            hw->ts_helper += played;
        }

        for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        again:
            if (!sw->active && sw->empty) {
                continue;
            }

            if (audio_bug (AUDIO_FUNC, played > sw->total_hw_samples_mixed)) {
                dolog ("played=%d sw->total_hw_samples_mixed=%d\n",
                       played, sw->total_hw_samples_mixed);
                played = sw->total_hw_samples_mixed;
            }

            sw->total_hw_samples_mixed -= played;

            if (!sw->total_hw_samples_mixed) {
                sw->empty = 1;

                if (!sw->active && !sw->callback.fn) {
                    SWVoiceOut *temp = sw->entries.le_next;

#ifdef DEBUG_PLIVE
                    dolog ("Finishing with old voice\n");
#endif
                    AUD_close_out (sw);
                    sw = temp;
                    if (sw) {
                        goto again;
                    }
                    else {
                        break;
                    }
                }
            }

            if (sw->active) {
                free = audio_get_free (sw);
                if (free > 0) {
                    sw->callback.fn (sw->callback.opaque, free);
                }
            }
        }
    }
}

static void audio_run_in (void)
{
    HWVoiceIn *hw = NULL;

    while ((hw = audio_pcm_hw_find_any_active_enabled_in (hw))) {
        SWVoiceIn *sw;
        int captured, min;

        captured = hw->pcm_ops->run_in (hw);

        min = audio_pcm_hw_find_min_in (hw);
        hw->total_samples_captured += captured - min;
        hw->ts_helper += captured;

        for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
            sw->total_hw_samples_acquired -= min;

            if (sw->active) {
                int avail;

                avail = audio_get_avail (sw);
                if (avail > 0) {
                    sw->callback.fn (sw->callback.opaque, avail);
                }
            }
        }
    }
}

static struct audio_option audio_options[] = {
    /* DAC */
    {"DAC_FIXED_SETTINGS", AUD_OPT_BOOL, &audio_state.fixed_settings_out,
     "Use fixed settings for host DAC", NULL, 0},

    {"DAC_FIXED_FREQ", AUD_OPT_INT, &audio_state.fixed_freq_out,
     "Frequency for fixed host DAC", NULL, 0},

    {"DAC_FIXED_FMT", AUD_OPT_FMT, &audio_state.fixed_fmt_out,
     "Format for fixed host DAC", NULL, 0},

    {"DAC_FIXED_CHANNELS", AUD_OPT_INT, &audio_state.fixed_channels_out,
     "Number of channels for fixed DAC (1 - mono, 2 - stereo)", NULL, 0},

    {"DAC_VOICES", AUD_OPT_INT, &audio_state.nb_hw_voices_out,
     "Number of voices for DAC", NULL, 0},

    /* ADC */
    {"ADC_FIXED_SETTINGS", AUD_OPT_BOOL, &audio_state.fixed_settings_out,
     "Use fixed settings for host ADC", NULL, 0},

    {"ADC_FIXED_FREQ", AUD_OPT_INT, &audio_state.fixed_freq_out,
     "Frequency for fixed ADC", NULL, 0},

    {"ADC_FIXED_FMT", AUD_OPT_FMT, &audio_state.fixed_fmt_out,
     "Format for fixed ADC", NULL, 0},

    {"ADC_FIXED_CHANNELS", AUD_OPT_INT, &audio_state.fixed_channels_in,
     "Number of channels for fixed ADC (1 - mono, 2 - stereo)", NULL, 0},

    {"ADC_VOICES", AUD_OPT_INT, &audio_state.nb_hw_voices_out,
     "Number of voices for ADC", NULL, 0},

    /* Misc */
    {"TIMER_PERIOD", AUD_OPT_INT, &audio_state.period.usec,
     "Timer period in microseconds (0 - try lowest possible)", NULL, 0},

    {"PLIVE", AUD_OPT_BOOL, &audio_state.plive,
     "(undocumented)", NULL, 0},

    {NULL, 0, NULL, NULL, NULL, 0}
};

void AUD_help (void)
{
    size_t i;

    audio_process_options ("AUDIO", audio_options);
    for (i = 0; i < sizeof (drvtab) / sizeof (drvtab[0]); i++) {
        struct audio_driver *d = drvtab[i];
        if (d->options) {
            audio_process_options (d->name, d->options);
        }
    }

    printf ("Audio options:\n");
    audio_print_options ("AUDIO", audio_options);
    printf ("\n");

    printf ("Available drivers:\n");

    for (i = 0; i < sizeof (drvtab) / sizeof (drvtab[0]); i++) {
        struct audio_driver *d = drvtab[i];

        printf ("Name: %s\n", d->name);
        printf ("Description: %s\n", d->descr);

        switch (d->max_voices_out) {
        case 0:
            printf ("Does not support DAC\n");
            break;
        case 1:
            printf ("One DAC voice\n");
            break;
        case INT_MAX:
            printf ("Theoretically supports many DAC voices\n");
            break;
        default:
            printf ("Theoretically supports upto %d DAC voices\n",
                     d->max_voices_out);
            break;
        }

        switch (d->max_voices_in) {
        case 0:
            printf ("Does not support ADC\n");
            break;
        case 1:
            printf ("One ADC voice\n");
            break;
        case INT_MAX:
            printf ("Theoretically supports many ADC voices\n");
            break;
        default:
            printf ("Theoretically supports upto %d ADC voices\n",
                     d->max_voices_in);
            break;
        }

        if (d->options) {
            printf ("Options:\n");
            audio_print_options (d->name, d->options);
        }
        else {
            printf ("No options\n");
        }
        printf ("\n");
    }

    printf (
        "Options are settable through environment variables.\n"
        "Example:\n"
#ifdef _WIN32
        "  set QEMU_AUDIO_DRV=wav\n"
        "  set QEMU_WAV_PATH=c:/tune.wav\n"
#else
        "  export QEMU_AUDIO_DRV=wav\n"
        "  export QEMU_WAV_PATH=$HOME/tune.wav\n"
        "(for csh replace export with setenv in the above)\n"
#endif
        "  qemu ...\n\n"
        );
}

void audio_timer (void *opaque)
{
    AudioState *s = opaque;

    audio_run_out ();
    audio_run_in ();

    qemu_mod_timer (s->ts, qemu_get_clock (vm_clock) + s->period.ticks);
}

static int audio_driver_init (struct audio_driver *drv)
{
    if (drv->options) {
        audio_process_options (drv->name, drv->options);
    }
    audio_state.opaque = drv->init ();

    if (audio_state.opaque) {
        int i;
        HWVoiceOut *hwo;
        HWVoiceIn *hwi;

        if (audio_state.nb_hw_voices_out > drv->max_voices_out) {
            if (!drv->max_voices_out) {
                dolog ("`%s' does not support DAC\n", drv->name);
            }
            else {
                dolog (
                    "`%s' does not support %d multiple DAC voicess\n"
                    "Resetting to %d\n",
                    drv->name,
                    audio_state.nb_hw_voices_out,
                    drv->max_voices_out
                    );
            }
            audio_state.nb_hw_voices_out = drv->max_voices_out;
        }

        LIST_INIT (&hw_head_out);
        hwo = qemu_mallocz (audio_state.nb_hw_voices_out * drv->voice_size_out);
        if (!hwo) {
            dolog (
                "Not enough memory for %d `%s' DAC voices (each %d bytes)\n",
                audio_state.nb_hw_voices_out,
                drv->name,
                drv->voice_size_out
                );
            drv->fini (audio_state.opaque);
            return -1;
        }

        for (i = 0; i < audio_state.nb_hw_voices_out; ++i) {
            LIST_INSERT_HEAD (&hw_head_out, hwo, entries);
            hwo = advance (hwo, drv->voice_size_out);
        }

        if (!drv->voice_size_in && drv->max_voices_in) {
            ldebug ("warning: No ADC voice size defined for `%s'\n",
                    drv->name);
            drv->max_voices_in = 0;
        }

        if (!drv->voice_size_out && drv->max_voices_out) {
            ldebug ("warning: No DAC voice size defined for `%s'\n",
                    drv->name);
        }

        if (drv->voice_size_in && !drv->max_voices_in) {
            ldebug ("warning: ADC voice size is %d for ADC less driver `%s'\n",
                    drv->voice_size_out, drv->name);
        }

        if (drv->voice_size_out && !drv->max_voices_out) {
            ldebug ("warning: DAC voice size is %d for DAC less driver `%s'\n",
                    drv->voice_size_in, drv->name);
        }

        if (audio_state.nb_hw_voices_in > drv->max_voices_in) {
            if (!drv->max_voices_in) {
                ldebug ("`%s' does not support ADC\n", drv->name);
            }
            else {
                dolog (
                    "`%s' does not support %d multiple ADC voices\n"
                    "Resetting to %d\n",
                    drv->name,
                    audio_state.nb_hw_voices_in,
                    drv->max_voices_in
                    );
            }
            audio_state.nb_hw_voices_in = drv->max_voices_in;
        }

        LIST_INIT (&hw_head_in);
        hwi = qemu_mallocz (audio_state.nb_hw_voices_in * drv->voice_size_in);
        if (!hwi) {
            dolog (
                "Not enough memory for %d `%s' ADC voices (each %d bytes)\n",
                audio_state.nb_hw_voices_in,
                drv->name,
                drv->voice_size_in
                );
            qemu_free (hwo);
            drv->fini (audio_state.opaque);
            return -1;
        }

        for (i = 0; i < audio_state.nb_hw_voices_in; ++i) {
            LIST_INSERT_HEAD (&hw_head_in, hwi, entries);
            hwi = advance (hwi, drv->voice_size_in);
        }

        audio_state.drv = drv;
        return 0;
    }
    else {
        dolog ("Could not init `%s' audio driver\n", drv->name);
        return -1;
    }
}

static void audio_vm_stop_handler (void *opaque, int reason)
{
    HWVoiceOut *hwo = NULL;
    HWVoiceIn *hwi = NULL;
    int op = reason ? VOICE_ENABLE : VOICE_DISABLE;

    (void) opaque;
    while ((hwo = audio_pcm_hw_find_any_out (hwo))) {
        if (!hwo->pcm_ops) {
            continue;
        }

        if (hwo->enabled != reason) {
            hwo->pcm_ops->ctl_out (hwo, op);
        }
    }

    while ((hwi = audio_pcm_hw_find_any_in (hwi))) {
        if (!hwi->pcm_ops) {
            continue;
        }

        if (hwi->enabled != reason) {
            hwi->pcm_ops->ctl_in (hwi, op);
        }
    }
}

static void audio_atexit (void)
{
    HWVoiceOut *hwo = NULL;
    HWVoiceIn *hwi = NULL;

    while ((hwo = audio_pcm_hw_find_any_out (hwo))) {
        if (!hwo->pcm_ops) {
            continue;
        }

        if (hwo->enabled) {
            hwo->pcm_ops->ctl_out (hwo, VOICE_DISABLE);
        }
        hwo->pcm_ops->fini_out (hwo);
    }

    while ((hwi = audio_pcm_hw_find_any_in (hwi))) {
        if (!hwi->pcm_ops) {
            continue;
        }

        if (hwi->enabled) {
            hwi->pcm_ops->ctl_in (hwi, VOICE_DISABLE);
        }
        hwi->pcm_ops->fini_in (hwi);
    }
    audio_state.drv->fini (audio_state.opaque);
}

static void audio_save (QEMUFile *f, void *opaque)
{
    (void) f;
    (void) opaque;
}

static int audio_load (QEMUFile *f, void *opaque, int version_id)
{
    (void) f;
    (void) opaque;

    if (version_id != 1) {
        return -EINVAL;
    }

    return 0;
}

void AUD_init (void)
{
    size_t i;
    int done = 0;
    const char *drvname;
    AudioState *s = &audio_state;

    audio_process_options ("AUDIO", audio_options);

    if (s->nb_hw_voices_out <= 0) {
        dolog ("Bogus number of DAC voices %d\n",
               s->nb_hw_voices_out);
        s->nb_hw_voices_out = 1;
    }

    if (s->nb_hw_voices_in <= 0) {
        dolog ("Bogus number of ADC voices %d\n",
               s->nb_hw_voices_in);
        s->nb_hw_voices_in = 1;
    }

    {
        int def;
        drvname = audio_get_conf_str ("QEMU_AUDIO_DRV", NULL, &def);
    }

    s->ts = qemu_new_timer (vm_clock, audio_timer, s);
    if (!s->ts) {
        dolog ("Can not create audio timer\n");
        return;
    }

    if (drvname) {
        int found = 0;

        for (i = 0; i < sizeof (drvtab) / sizeof (drvtab[0]); i++) {
            if (!strcmp (drvname, drvtab[i]->name)) {
                done = !audio_driver_init (drvtab[i]);
                found = 1;
                break;
            }
        }

        if (!found) {
            dolog ("Unknown audio driver `%s'\n", drvname);
            dolog ("Run with -audio-help to list available drivers\n");
        }
    }

    qemu_add_vm_stop_handler (audio_vm_stop_handler, NULL);
    atexit (audio_atexit);

    if (!done) {
        for (i = 0; !done && i < sizeof (drvtab) / sizeof (drvtab[0]); i++) {
            if (drvtab[i]->can_be_default) {
                done = !audio_driver_init (drvtab[i]);
            }
        }
    }

    register_savevm ("audio", 0, 1, audio_save, audio_load, NULL);
    if (!done) {
        if (audio_driver_init (&no_audio_driver)) {
            dolog ("Can not initialize audio subsystem\n");
        }
        else {
            dolog ("warning: using timer based audio emulation\n");
        }
    }

    if (s->period.usec <= 0) {
        if (s->period.usec < 0) {
            dolog ("warning: timer period is negative - %d treating as zero\n",
                   s->period.usec);
        }
        s->period.ticks = 1;
    }
    else {
        s->period.ticks = (ticks_per_sec * s->period.usec) / 1000000;
    }

    qemu_mod_timer (s->ts, qemu_get_clock (vm_clock) + s->period.ticks);
}
