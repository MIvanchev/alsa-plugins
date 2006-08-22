/*
 * ALSA <-> PulseAudio mixer control plugin
 *
 * Copyright (c) 2006 by Pierre Ossman <ossman@cendio.se>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <sys/poll.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include "pulse.h"

typedef struct snd_ctl_pulse {
	snd_ctl_ext_t ext;

    snd_pulse_t *p;

    char *source;
    char *sink;

    pa_cvolume sink_volume;
    pa_cvolume source_volume;

    int sink_muted;
    int source_muted;

    int subscribed;
    int updated;
} snd_ctl_pulse_t;

#define SOURCE_VOL_NAME "Capture Volume"
#define SOURCE_MUTE_NAME "Capture Switch"
#define SINK_VOL_NAME "Master Playback Volume"
#define SINK_MUTE_NAME "Master Playback Switch"

#define UPDATE_SINK_VOL     0x01
#define UPDATE_SINK_MUTE    0x02
#define UPDATE_SOURCE_VOL   0x04
#define UPDATE_SOURCE_MUTE  0x08

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    snd_ctl_pulse_t *ctl = (snd_ctl_pulse_t*)userdata;
    int chan;

    assert(ctl);

    if (is_last) {
        pa_threaded_mainloop_signal(ctl->p->mainloop, 0);
        return;
    }

    assert(i);

    if (!!ctl->sink_muted != !!i->mute) {
        ctl->sink_muted = i->mute;
        ctl->updated |= UPDATE_SINK_MUTE;
        pulse_poll_activate(ctl->p);
    }

    if (ctl->sink_volume.channels == i->volume.channels) {
        for (chan = 0;chan < ctl->sink_volume.channels;chan++)
            if (i->volume.values[chan] != ctl->sink_volume.values[chan])
                break;

        if (chan == ctl->sink_volume.channels)
            return;

        ctl->updated |= UPDATE_SINK_VOL;
        pulse_poll_activate(ctl->p);
    }

    memcpy(&ctl->sink_volume, &i->volume, sizeof(pa_cvolume));
}

static void source_info_cb(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    snd_ctl_pulse_t *ctl = (snd_ctl_pulse_t*)userdata;
    int chan;

    assert(ctl);

    if (is_last) {
        pa_threaded_mainloop_signal(ctl->p->mainloop, 0);
        return;
    }

    assert(i);

    if (!!ctl->source_muted != !!i->mute) {
        ctl->source_muted = i->mute;
        ctl->updated |= UPDATE_SOURCE_MUTE;
        pulse_poll_activate(ctl->p);
    }

    if (ctl->source_volume.channels == i->volume.channels) {
        for (chan = 0;chan < ctl->source_volume.channels;chan++)
            if (i->volume.values[chan] != ctl->source_volume.values[chan])
                break;

        if (chan == ctl->source_volume.channels)
            return;

        ctl->updated |= UPDATE_SOURCE_VOL;
        pulse_poll_activate(ctl->p);
    }

    memcpy(&ctl->source_volume, &i->volume, sizeof(pa_cvolume));
}

static void event_cb(pa_context *c, pa_subscription_event_type_t t,
    uint32_t index, void *userdata)
{
    snd_ctl_pulse_t *ctl = (snd_ctl_pulse_t*)userdata;
    pa_operation *o;

    assert(ctl && ctl->p && ctl->p->context);

    o = pa_context_get_sink_info_by_name(ctl->p->context, ctl->sink,
        sink_info_cb, ctl);
    pa_operation_unref(o);

    o = pa_context_get_source_info_by_name(ctl->p->context, ctl->source,
        source_info_cb, ctl);
    pa_operation_unref(o);
}

static int pulse_update_volume(snd_ctl_pulse_t *ctl)
{
    int err;
    pa_operation *o;

    assert(ctl && ctl->p && ctl->p->context);

    o = pa_context_get_sink_info_by_name(ctl->p->context, ctl->sink,
        sink_info_cb, ctl);
    err = pulse_wait_operation(ctl->p, o);
    pa_operation_unref(o);
    if (err < 0)
        return err;

    o = pa_context_get_source_info_by_name(ctl->p->context, ctl->source,
        source_info_cb, ctl);
    err = pulse_wait_operation(ctl->p, o);
    pa_operation_unref(o);
    if (err < 0)
        return err;

    return 0;
}

static int pulse_elem_count(snd_ctl_ext_t *ext)
{
    snd_ctl_pulse_t *ctl = ext->private_data;
    int count = 0;

    assert(ctl);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    if (ctl->source)
        count += 2;
    if (ctl->sink)
        count += 2;

    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return count;
}

static int pulse_elem_list(snd_ctl_ext_t *ext, unsigned int offset,
    snd_ctl_elem_id_t *id)
{
    snd_ctl_pulse_t *ctl = ext->private_data;

    assert(ctl);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    if (ctl->source) {
        if (offset == 0)
            snd_ctl_elem_id_set_name(id, SOURCE_VOL_NAME);
        else if (offset == 1)
            snd_ctl_elem_id_set_name(id, SOURCE_MUTE_NAME);
    } else
        offset += 2;

    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    if (offset == 2)
        snd_ctl_elem_id_set_name(id, SINK_VOL_NAME);
    else if (offset == 3)
        snd_ctl_elem_id_set_name(id, SINK_MUTE_NAME);

    return 0;
}

static snd_ctl_ext_key_t pulse_find_elem(snd_ctl_ext_t *ext,
    const snd_ctl_elem_id_t *id)
{
    const char *name;

    name = snd_ctl_elem_id_get_name(id);

    if (strcmp(name, SOURCE_VOL_NAME) == 0)
        return 0;
    if (strcmp(name, SOURCE_MUTE_NAME) == 0)
        return 1;
    if (strcmp(name, SINK_VOL_NAME) == 0)
        return 2;
    if (strcmp(name, SINK_MUTE_NAME) == 0)
        return 3;

    return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int pulse_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
    int *type, unsigned int *acc, unsigned int *count)
{
    snd_ctl_pulse_t *ctl = ext->private_data;
    int err = 0;

    if (key > 3)
        return -EINVAL;

    assert(ctl);
    assert(ctl->p);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    err = pulse_check_connection(ctl->p);
    if (err < 0)
        goto finish;

    err = pulse_update_volume(ctl);
    if (err < 0)
        goto finish;

    if (key & 1)
        *type = SND_CTL_ELEM_TYPE_BOOLEAN;
    else
        *type = SND_CTL_ELEM_TYPE_INTEGER;

    *acc = SND_CTL_EXT_ACCESS_READWRITE;

    if (key == 0)
        *count = ctl->source_volume.channels;
    else if (key == 2)
        *count = ctl->sink_volume.channels;
    else
        *count = 1;

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return err;
}

static int pulse_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
    long *imin, long *imax, long *istep)
{
    *istep = 1;
    *imin = 0;
    *imax = PA_VOLUME_NORM;

    return 0;
}

static int pulse_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
    long *value)
{
    snd_ctl_pulse_t *ctl = ext->private_data;
    int err = 0, i;
    pa_cvolume *vol = NULL;

    assert(ctl);
    assert(ctl->p);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    err = pulse_check_connection(ctl->p);
    if (err < 0)
        goto finish;

    err = pulse_update_volume(ctl);
    if (err < 0)
        goto finish;

    switch (key) {
    case 0:
        vol = &ctl->source_volume;
        break;
    case 1:
        *value = !ctl->source_muted;
        break;
    case 2:
        vol = &ctl->sink_volume;
        break;
    case 3:
        *value = !ctl->sink_muted;
        break;
    default:
        err = -EINVAL;
        goto finish;
    }

    if (vol) {
        for (i = 0;i < vol->channels;i++)
            value[i] = vol->values[i];
    }

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return err;
}

static int pulse_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
    long *value)
{
    snd_ctl_pulse_t *ctl = ext->private_data;
    int err = 0, i;
    pa_operation *o;
    pa_cvolume *vol = NULL;

    assert(ctl);
    assert(ctl->p && ctl->p->context);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    err = pulse_check_connection(ctl->p);
    if (err < 0)
        goto finish;

    err = pulse_update_volume(ctl);
    if (err < 0)
        goto finish;

    switch (key) {
    case 0:
        vol = &ctl->source_volume;
        break;
    case 1:
        if (!!ctl->source_muted == !*value)
            goto finish;
        ctl->source_muted = !*value;
        break;
    case 2:
        vol = &ctl->sink_volume;
        break;
    case 3:
        if (!!ctl->sink_muted == !*value)
            goto finish;
        ctl->sink_muted = !*value;
        break;
    default:
        err = -EINVAL;
        goto finish;
    }

    if (vol) {
        for (i = 0;i < vol->channels;i++)
            if (value[i] != vol->values[i])
                break;

        if (i == vol->channels)
            goto finish;

        for (i = 0;i < vol->channels;i++)
            vol->values[i] = value[i];

        if (key == 0)
            o = pa_context_set_source_volume_by_name(ctl->p->context,
                ctl->source, vol, pulse_context_success_cb, ctl->p);
        else
            o = pa_context_set_sink_volume_by_name(ctl->p->context,
                ctl->sink, vol, pulse_context_success_cb, ctl->p);
    } else {
        if (key == 1)
            o = pa_context_set_source_mute_by_name(ctl->p->context,
                ctl->source, ctl->source_muted, pulse_context_success_cb, ctl->p);
        else
            o = pa_context_set_sink_mute_by_name(ctl->p->context,
                ctl->sink, ctl->sink_muted, pulse_context_success_cb, ctl->p);
    }

    err = pulse_wait_operation(ctl->p, o);
    pa_operation_unref(o);
    if (err < 0)
        goto finish;

    err = 1;

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return err;
}

static void pulse_subscribe_events(snd_ctl_ext_t *ext, int subscribe)
{
    snd_ctl_pulse_t *ctl = ext->private_data;

    assert(ctl);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    ctl->subscribed = !!(subscribe & SND_CTL_EVENT_MASK_VALUE);

    pa_threaded_mainloop_unlock(ctl->p->mainloop);
}

static int pulse_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id,
    unsigned int *event_mask)
{
    snd_ctl_pulse_t *ctl = ext->private_data;
    int offset;
    int err = -EAGAIN;

    assert(ctl);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    if (!ctl->updated || !ctl->subscribed)
        goto finish;

    if (ctl->source)
        offset = 2;
    else
        offset = 0;

    if (ctl->updated & UPDATE_SOURCE_VOL) {
        pulse_elem_list(ext, 0, id);
        ctl->updated &= ~UPDATE_SOURCE_VOL;
    } else if (ctl->updated & UPDATE_SOURCE_MUTE) {
        pulse_elem_list(ext, 1, id);
        ctl->updated &= ~UPDATE_SOURCE_MUTE;
    } else if (ctl->updated & UPDATE_SINK_VOL) {
        pulse_elem_list(ext, offset + 0, id);
        ctl->updated &= ~UPDATE_SINK_VOL;
    } else if (ctl->updated & UPDATE_SINK_MUTE) {
        pulse_elem_list(ext, offset + 1, id);
        ctl->updated &= ~UPDATE_SINK_MUTE;
    }

    *event_mask = SND_CTL_EVENT_MASK_VALUE;

    if (!ctl->updated)
        pulse_poll_deactivate(ctl->p);

    err = 1;

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return err;
}

static int pulse_ctl_poll_descriptors_count(snd_ctl_ext_t *ext)
{
	snd_ctl_pulse_t *ctl = ext->private_data;
	int count;

    assert(ctl);
    assert(ctl->p);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    count = pulse_poll_descriptors_count(ctl->p);

    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return count;
}

static int pulse_ctl_poll_descriptors(snd_ctl_ext_t *ext, struct pollfd *pfd, unsigned int space)
{
    int num;

	snd_ctl_pulse_t *ctl = ext->private_data;

    assert(ctl);
    assert(ctl->p);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    num = pulse_poll_descriptors(ctl->p, pfd, space);
    if (num < 0)
        goto finish;

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return num;
}

static int pulse_ctl_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
	snd_ctl_pulse_t *ctl = ext->private_data;
	int err = 0;

    assert(ctl);
    assert(ctl->p);

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    err = pulse_poll_revents(ctl->p, pfd, nfds, revents);
    if (err < 0)
        goto finish;

    *revents = 0;

    if (ctl->updated)
        *revents |= POLLIN;

finish:
    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    return err;
}

static void pulse_close(snd_ctl_ext_t *ext)
{
    snd_ctl_pulse_t *ctl = ext->private_data;

    assert(ctl);

    if (ctl->p)
        pulse_free(ctl->p);

    if (ctl->source)
        free(ctl->source);
    if (ctl->sink)
        free(ctl->sink);

	free(ctl);
}

static snd_ctl_ext_callback_t pulse_ext_callback = {
    .elem_count = pulse_elem_count,
    .elem_list = pulse_elem_list,
    .find_elem = pulse_find_elem,
    .get_attribute = pulse_get_attribute,
    .get_integer_info = pulse_get_integer_info,
    .read_integer = pulse_read_integer,
    .write_integer = pulse_write_integer,
    .subscribe_events = pulse_subscribe_events,
    .read_event = pulse_read_event,
    .poll_descriptors_count = pulse_ctl_poll_descriptors_count,
    .poll_descriptors = pulse_ctl_poll_descriptors,
    .poll_revents = pulse_ctl_poll_revents,
    .close = pulse_close,
};

static void server_info_cb(pa_context *c, const pa_server_info*i, void *userdata)
{
    snd_ctl_pulse_t *ctl = (snd_ctl_pulse_t*)userdata;

    assert(ctl && i);

    if (i->default_source_name && !ctl->source)
        ctl->source = strdup(i->default_source_name);
    if (i->default_sink_name && !ctl->sink)
        ctl->sink = strdup(i->default_sink_name);

    pa_threaded_mainloop_signal(ctl->p->mainloop, 0);
}

SND_CTL_PLUGIN_DEFINE_FUNC(pulse)
{
	snd_config_iterator_t i, next;
	const char *server = NULL;
	const char *device = NULL;
	const char *source = NULL;
	const char *sink = NULL;
	int err;
	snd_ctl_pulse_t *ctl;
    pa_operation *o;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
			continue;
        if (strcmp(id, "server") == 0) {
            if (snd_config_get_string(n, &server) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "device") == 0) {
            if (snd_config_get_string(n, &device) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "source") == 0) {
            if (snd_config_get_string(n, &source) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "sink") == 0) {
            if (snd_config_get_string(n, &sink) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            continue;
        }
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	ctl = calloc(1, sizeof(*ctl));

    ctl->p = pulse_new();
    if (!ctl->p) {
        err = -EIO;
        goto error;
    }

    err = pulse_connect(ctl->p, server);
    if (err < 0)
        goto error;

    if (source)
        ctl->source = strdup(source);
    else if (device)
        ctl->source = strdup(device);

    if (sink)
        ctl->sink = strdup(sink);
    else if (device)
        ctl->sink = strdup(device);

    if (!ctl->source || !ctl->sink) {
        pa_threaded_mainloop_lock(ctl->p->mainloop);

        o = pa_context_get_server_info(ctl->p->context, server_info_cb, ctl);
        err = pulse_wait_operation(ctl->p, o);

        pa_operation_unref(o);

        pa_threaded_mainloop_unlock(ctl->p->mainloop);

        if (err < 0)
            goto error;
    }

    pa_threaded_mainloop_lock(ctl->p->mainloop);

    pa_context_set_subscribe_callback(ctl->p->context, event_cb, ctl);

    o = pa_context_subscribe(ctl->p->context,
        PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE,
        pulse_context_success_cb, ctl->p);

    err = pulse_wait_operation(ctl->p, o);

    pa_operation_unref(o);

    pa_threaded_mainloop_unlock(ctl->p->mainloop);

    if (err < 0)
        goto error;

    ctl->ext.version = SND_CTL_EXT_VERSION;
    ctl->ext.card_idx = 0;
    strncpy(ctl->ext.id, "pulse", sizeof(ctl->ext.id) - 1);
    strncpy(ctl->ext.driver, "Polypaudio plugin", sizeof(ctl->ext.driver) - 1);
    strncpy(ctl->ext.name, "Polypaudio", sizeof(ctl->ext.name) - 1);
    strncpy(ctl->ext.longname, "Polypaudio", sizeof(ctl->ext.longname) - 1);
    strncpy(ctl->ext.mixername, "Polypaudio", sizeof(ctl->ext.mixername) - 1);
    ctl->ext.poll_fd = -1;
    ctl->ext.callback = &pulse_ext_callback;
    ctl->ext.private_data = ctl;

    err = snd_ctl_ext_create(&ctl->ext, name, mode);
    if (err < 0)
        goto error;

    *handlep = ctl->ext.handle;

    return 0;

error:
    if (ctl->source)
        free(ctl->source);
    if (ctl->sink)
        free(ctl->sink);

    if (ctl->p)
        pulse_free(ctl->p);

	free(ctl);

	return err;
}

SND_CTL_PLUGIN_SYMBOL(pulse);