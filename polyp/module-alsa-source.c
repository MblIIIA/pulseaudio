/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <sys/poll.h>

#include <asoundlib.h>

#include "module.h"
#include "core.h"
#include "memchunk.h"
#include "sink.h"
#include "modargs.h"
#include "util.h"
#include "sample-util.h"
#include "alsa-util.h"
#include "xmalloc.h"

struct userdata {
    snd_pcm_t *pcm_handle;
    struct pa_source *source;
    struct pa_io_event **io_events;
    unsigned n_io_events;

    size_t frame_size, fragment_size;
    struct pa_memchunk memchunk;
    struct pa_module *module;
};

static const char* const valid_modargs[] = {
    "device",
    "source_name",
    "format",
    "channels",
    "rate",
    "fragments",
    "fragment_size",
    NULL
};

#define DEFAULT_SOURCE_NAME "alsa_input"
#define DEFAULT_DEVICE "hw:0,0"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->source ? pa_idxset_ncontents(u->source->outputs) : 0));
}

static void xrun_recovery(struct userdata *u) {
    assert(u);

    fprintf(stderr, "*** ALSA-XRUN (capture) ***\n");
    
    if (snd_pcm_prepare(u->pcm_handle) < 0)
        fprintf(stderr, "snd_pcm_prepare() failed\n");
}

static void do_read(struct userdata *u) {
    assert(u);

    update_usage(u);
    
    for (;;) {
        struct pa_memchunk post_memchunk;
        snd_pcm_sframes_t frames;
        size_t l;
        
        if (!u->memchunk.memblock) {
            u->memchunk.memblock = pa_memblock_new(u->memchunk.length = u->fragment_size, u->source->core->memblock_stat);
            u->memchunk.index = 0;
        }
            
        assert(u->memchunk.memblock && u->memchunk.memblock->data && u->memchunk.length && u->memchunk.memblock->length && (u->memchunk.length % u->frame_size) == 0);

        if ((frames = snd_pcm_readi(u->pcm_handle, (uint8_t*) u->memchunk.memblock->data + u->memchunk.index, u->memchunk.length / u->frame_size)) < 0) {
            if (frames == -EAGAIN)
                return;
            
            if (frames == -EPIPE) {
                xrun_recovery(u);
                continue;
            }

            fprintf(stderr, "snd_pcm_readi() failed: %s\n", strerror(-frames));
            return;
        }

        l = frames * u->frame_size;
        
        post_memchunk = u->memchunk;
        post_memchunk.length = l;

        pa_source_post(u->source, &post_memchunk);

        u->memchunk.index += l;
        u->memchunk.length -= l;
        
        if (u->memchunk.length == 0) {
            pa_memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
            u->memchunk.index = u->memchunk.length = 0;
        }
        
        break;
    }
}

static void io_callback(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct userdata *u = userdata;
    assert(u && a && e);

    if (snd_pcm_state(u->pcm_handle) == SND_PCM_STATE_XRUN)
        xrun_recovery(u);

    do_read(u);
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    const char *dev;
    struct pa_sample_spec ss;
    unsigned periods, fragsize;
    snd_pcm_uframes_t buffer_size;
    size_t frame_size;
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        fprintf(stderr, __FILE__": failed to parse sample specification\n");
        goto fail;
    }
    frame_size = pa_frame_size(&ss);
    
    periods = 12;
    fragsize = 1024;
    if (pa_modargs_get_value_u32(ma, "fragments", &periods) < 0 || pa_modargs_get_value_u32(ma, "fragment_size", &fragsize) < 0) {
        fprintf(stderr, __FILE__": failed to parse buffer metrics\n");
        goto fail;
    }
    buffer_size = fragsize/frame_size*periods;
    
    u = pa_xmalloc0(sizeof(struct userdata));
    m->userdata = u;
    u->module = m;
    
    if (snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0) {
        fprintf(stderr, __FILE__": Error opening PCM device %s\n", dev);
        goto fail;
    }

    if (pa_alsa_set_hw_params(u->pcm_handle, &ss, &periods, &buffer_size) < 0) {
        fprintf(stderr, __FILE__": Failed to set hardware parameters\n");
        goto fail;
    }

    u->source = pa_source_new(c, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss);
    assert(u->source);

    u->source->userdata = u;
    pa_source_set_owner(u->source, m);
    u->source->description = pa_sprintf_malloc("Advanced Linux Sound Architecture PCM on '%s'", dev);

    if (pa_create_io_events(u->pcm_handle, c->mainloop, &u->io_events, &u->n_io_events, io_callback, u) < 0) {
        fprintf(stderr, __FILE__": failed to obtain file descriptors\n");
        goto fail;
    }

    u->frame_size = frame_size;
    u->fragment_size = buffer_size*u->frame_size/periods;

    fprintf(stderr, __FILE__": using %u fragments of size %u bytes.\n", periods, u->fragment_size);

    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;

    snd_pcm_start(u->pcm_handle);
    
    ret = 0;

finish:
     if (ma)
         pa_modargs_free(ma);
    
    return ret;

fail:
    
    if (u)
        pa_module_done(c, m);

    goto finish;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->source)
        pa_source_free(u->source);
    
    if (u->io_events)
        pa_free_io_events(c->mainloop, u->io_events, u->n_io_events);
    
    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
    }
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    
    pa_xfree(u);
}

