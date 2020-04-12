/*
Copyright (C) 2020 Ed Maste <emaste@freebsd.org>
Copyright (C) 2015. Guillermo A. Amaral B. <g@maral.me>

Based on ALSA Input plugin by Guillermo A. Amaral B., which is based on
Pulse Input plugin by Leonhard Oelke.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
int close(int fd); // XXX
ssize_t read(int fd, void *buf, size_t nbytes); // XXX

#include <sys/soundcard.h>

#define blog(level, msg, ...) blog(level, "oss-input: " msg, ##__VA_ARGS__)

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000L
#define STARTUP_TIMEOUT_NS (500 * NSEC_PER_MSEC)
#define REOPEN_TIMEOUT 1000UL
#define SHUTDOWN_ON_DEACTIVATE false

struct oss_data {
	obs_source_t *source;
#if SHUTDOWN_ON_DEACTIVATE
	bool active;
#endif

	/* user settings */
	char *device;

	/* pthread */
	pthread_t listen_thread;
	pthread_t reopen_thread;
	os_event_t *abort_event;
	volatile bool listen;
	volatile bool reopen;

	/* oss */
	int fd;
	int format;

	size_t period_size;
	unsigned int channels;
	unsigned int rate;
	unsigned int sample_size;
	uint8_t *buffer;
	uint64_t first_ts;
};

static const char *oss_get_name(void *);
static bool oss_devices_changed(obs_properties_t *props, obs_property_t *p,
				 obs_data_t *settings);
static obs_properties_t *oss_get_properties(void *);
static void *oss_create(obs_data_t *, obs_source_t *);
static void oss_destroy(void *);
static void oss_activate(void *);
static void oss_deactivate(void *);
static void oss_get_defaults(obs_data_t *);
static void oss_update(void *, obs_data_t *);

struct obs_source_info oss_input_capture = {
	.id = "oss_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.create = oss_create,
	.destroy = oss_destroy,
#if SHUTDOWN_ON_DEACTIVATE
	.activate = oss_activate,
	.deactivate = oss_deactivate,
#endif
	.update = oss_update,
	.get_defaults = oss_get_defaults,
	.get_name = oss_get_name,
	.get_properties = oss_get_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};

static bool _oss_try_open(struct oss_data *);
static bool _oss_open(struct oss_data *);
static void _oss_close(struct oss_data *);
static bool _oss_configure(struct oss_data *);
static void _oss_start_reopen(struct oss_data *);
static void _oss_stop_reopen(struct oss_data *);
static void *_oss_listen(void *);
static void *_oss_reopen(void *);

static enum audio_format _oss_to_obs_audio_format(int);
static enum speaker_layout _oss_channels_to_obs_speakers(unsigned int);

/*****************************************************************************/

void *oss_create(obs_data_t *settings, obs_source_t *source)
{
	struct oss_data *data = bzalloc(sizeof(struct oss_data));

	data->source = source;
#if SHUTDOWN_ON_DEACTIVATE
	data->active = false;
#endif
	data->buffer = NULL;
	data->device = NULL;
	data->first_ts = 0;
	data->fd = -1;
	data->listen = false;
	data->reopen = false;
	data->listen_thread = 0;
	data->reopen_thread = 0;

	const char *device = obs_data_get_string(settings, "device_id");

	data->device = bstrdup(device);
	data->rate = obs_data_get_int(settings, "rate");

	if (os_event_init(&data->abort_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "Abort event creation failed!");
		goto cleanup;
	}

#if !SHUTDOWN_ON_DEACTIVATE
	_oss_try_open(data);
#endif
	return data;

cleanup:
	if (data->device)
		bfree(data->device);

	bfree(data);
	return NULL;
}

void oss_destroy(void *vptr)
{
	struct oss_data *data = vptr;

	if (data->fd == -1)
		_oss_close(data);

	os_event_destroy(data->abort_event);
	bfree(data->device);
	bfree(data);
}

#if SHUTDOWN_ON_DEACTIVATE
void oss_activate(void *vptr)
{
	struct oss_data *data = vptr;

	data->active = true;
	_oss_try_open(data);
}

void oss_deactivate(void *vptr)
{
	struct oss_data *data = vptr;

	_oss_stop_reopen(data);
	_oss_close(data);
	data->active = false;
}
#endif

void oss_update(void *vptr, obs_data_t *settings)
{
	struct oss_data *data = vptr;
	const char *device;
	unsigned int rate;
	bool reset = false;

	device = obs_data_get_string(settings, "device_id");
	blog(LOG_INFO, "device %s", device);

	if (strcmp(data->device, device) != 0) {
		bfree(data->device);
		data->device = bstrdup(device);
		reset = true;
	}

	rate = obs_data_get_int(settings, "rate");
	if (data->rate != rate) {
		data->rate = rate;
		reset = true;
	}

#if SHUTDOWN_ON_DEACTIVATE
	if (reset && data->fd == -1)
		_oss_close(data);

	if (data->active && !data->fd != -1)
		_oss_try_open(data);
#else
	if (reset) {
		if (data->fd != -1)
			_oss_close(data);
		_oss_try_open(data);
	}
#endif
}

const char *oss_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OSS Input");
}

void oss_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device_id", "/dev/dsp0");
	obs_data_set_default_int(settings, "rate", 44100);
}

static bool oss_devices_changed(obs_properties_t *props, obs_property_t *p,
				 obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	bool visible = false;
	const char *device_id = obs_data_get_string(settings, "device_id");

	return true;
}

obs_properties_t *oss_get_properties(void *unused)
{
//	void **hints;
//	void **hint;
//	char *name = NULL;
	char *descr = NULL;
//	char *io = NULL;
//	char *descr_i;
	obs_properties_t *props;
	obs_property_t *devices;
	obs_property_t *rate;
	FILE *f;
	char *devname;
//
	UNUSED_PARAMETER(unused);

	if ((f = fopen("/dev/sndstat", "r")) == NULL)
		return NULL;

	props = obs_properties_create();

	devices = obs_properties_add_list(props, "device_id",
					  obs_module_text("Device"),
					  OBS_COMBO_TYPE_LIST,
					  OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(devices, "Default", "/dev/dsp0");

	while (!feof(f)) {
		char line[1024];
		char *pdesc, *pmode, *p;
		int pcm;
		if (!fgets(line, sizeof(line), f))
			break;
		if (sscanf(line, "pcm%i: ", &pcm) != 1)
			continue;
		if ((p = strchr(line, '<')) == NULL)
			continue;
		pdesc = p + 1;
		if ((p = strrchr(pdesc, '>')) == NULL)
			continue;
		*p++ = '\0';
		if (*p++ != ' ' || *p++ != '(')
			continue;
		pmode = p;
		if ((p = strrchr(pmode, ')')) == NULL)
			continue;
		*p++ = '\0';
		if (strcmp(pmode, "rec") != 0 && strcmp(pmode, "play/rec") != 0)
			continue;
		asprintf(&descr, "pcm%i: %s", pcm, pdesc);
		asprintf(&devname, "/dev/dsp%i", pcm);
		obs_property_list_add_string(devices, descr, devname);
		free(descr);
	}
	fclose(f);

	rate = obs_properties_add_list(props, "rate", obs_module_text("Rate"),
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);

	obs_property_set_modified_callback(devices, oss_devices_changed);

	obs_property_list_add_int(rate, "32000 Hz", 32000);
	obs_property_list_add_int(rate, "44100 Hz", 44100);
	obs_property_list_add_int(rate, "48000 Hz", 48000);
//
//	if (snd_device_name_hint(-1, "pcm", &hints) < 0)
//		return props;
//
//	hint = hints;
//	while (*hint != NULL) {
//		/* check if we're dealing with an Input */
//		io = snd_device_name_get_hint(*hint, "IOID");
//		if (io != NULL && strcmp(io, "Input") != 0)
//			goto next;
//
//		name = snd_device_name_get_hint(*hint, "NAME");
//		if (name == NULL || strstr(name, "front:") == NULL)
//			goto next;
//
//		descr = snd_device_name_get_hint(*hint, "DESC");
//		if (!descr)
//			goto next;
//
//		descr_i = descr;
//		while (*descr_i) {
//			if (*descr_i == '\n') {
//				*descr_i = '\0';
//				break;
//			} else
//				++descr_i;
//		}
//
//		obs_property_list_add_string(devices, descr, name);
//
//	next:
//		if (name != NULL)
//			free(name), name = NULL;
//
//		if (descr != NULL)
//			free(descr), descr = NULL;
//
//		if (io != NULL)
//			free(io), io = NULL;
//
//		++hint;
//	}
//	obs_property_list_add_string(devices, "Custom", "__custom__");
//
//	snd_device_name_free_hint(hints);
//
	return props;
}

/*****************************************************************************/

bool _oss_try_open(struct oss_data *data)
{
	_oss_stop_reopen(data);

	if (_oss_open(data))
		return true;

	_oss_start_reopen(data);

	return false;
}

bool _oss_open(struct oss_data *data)
{
	pthread_attr_t attr;
	int err;

	if ((data->fd = open(data->device, 0600)) < 0) {
		blog(LOG_ERROR, "Failed to open '%s': %s", data->device,
		     strerror(errno));
		return false;
	}
	if (!_oss_configure(data))
		goto cleanup;
//
//	if (snd_pcm_state(data->handle) != SND_PCM_STATE_PREPARED) {
//		blog(LOG_ERROR, "Device not prepared: '%s'", data->device);
////		goto cleanup;
//	}
//
//	/* start listening */
//
////	err = snd_pcm_start(data->handle);
//	if (err < 0) {
//		blog(LOG_ERROR, "Failed to start '%s': %s", data->device,
//		     snd_strerror(err));
//		goto cleanup;
//	}

	/* create capture thread */

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	err = pthread_create(&data->listen_thread, &attr, _oss_listen, data);
	if (err) {
		pthread_attr_destroy(&attr);
		blog(LOG_ERROR,
		     "Failed to create capture thread for device '%s'.",
		     data->device);
		goto cleanup;
	}

	pthread_attr_destroy(&attr);
	return true;

cleanup:
	_oss_close(data);
	return false;
}

void _oss_close(struct oss_data *data)
{
	(void)data;
	if (data->listen_thread) {
		os_atomic_set_bool(&data->listen, false);
		pthread_join(data->listen_thread, NULL);
		data->listen_thread = 0;
	}

	if (data->fd != -1) {
		close(data->fd);
		data->fd = -1;
	}

	if (data->buffer)
		bfree(data->buffer), data->buffer = NULL;
}

bool _oss_configure(struct oss_data *data)
{
//	snd_pcm_hw_params_t *hwparams;
//	int err;
//	int dir;
	int val;
//
//	snd_pcm_hw_params_alloca(&hwparams);
//
//	err = snd_pcm_hw_params_any(data->handle, hwparams);
//	if (err < 0) {
//		blog(LOG_ERROR, "snd_pcm_hw_params_any failed: %s", "");
//		return false;
//	}
//
//	err = snd_pcm_hw_params_set_access(data->handle, hwparams,
//					   SND_PCM_ACCESS_RW_INTERLEAVED);
//	if (err < 0) {
//		blog(LOG_ERROR, "snd_pcm_hw_params_set_access failed: %s",
//		     snd_strerror(err));
//		return false;
//	}
//
	data->format = AFMT_S16_NE;
	if (ioctl(data->fd, SNDCTL_DSP_SETFMT, &data->format) == -1) {
		blog(LOG_ERROR, "SNDCTL_DSP_SETFMT failed: %s",
		     strerror(errno));
		return false;
	}

//	err = snd_pcm_hw_params_get_channels(hwparams, &data->channels);
//	if (err < 0)
		data->channels = 2;
//
//	err = snd_pcm_hw_params_set_channels_near(data->handle, hwparams,
//						  &data->channels);
	if (ioctl(data->fd, SNDCTL_DSP_CHANNELS, &data->channels) == -1) {
		blog(LOG_ERROR,
		     "SNDCTL_DSP_CHANNELS failed: %s", strerror(errno));
		return false;
	}
	blog(LOG_INFO, "PCM '%s' channels set to %d", data->device,
	     data->channels);

	if (ioctl(data->fd, SNDCTL_DSP_SPEED, &data->rate) == -1) {
		blog(LOG_ERROR, "set SNDCTL_DSP_SPEED %d failed: %s",
		     data->rate, strerror(errno));
		return false;
	}
	blog(LOG_INFO, "PCM '%s' rate set to %d", data->device, data->rate);

//	err = snd_pcm_hw_params(data->handle, hwparams);
//	if (err < 0) {
//		blog(LOG_ERROR, "snd_pcm_hw_params failed: %s",
//		     snd_strerror(err));
//		return false;
//	}
//
//	err = snd_pcm_hw_params_get_period_size(hwparams, &data->period_size,
//						&dir);
//	if (err < 0) {
//		blog(LOG_ERROR, "snd_pcm_hw_params_get_period_size failed: %s",
//		     snd_strerror(err));
//		return false;
//	}
//
	data->sample_size =
		data->channels * 16 / 8;

	data->period_size = 16384;
	data->sample_size = 4;
	if (data->buffer)
		bfree(data->buffer);
	data->buffer = bzalloc(data->period_size * data->sample_size);

	return true;
}

void _oss_start_reopen(struct oss_data *data)
{
	pthread_attr_t attr;
	int err;

	if (os_atomic_load_bool(&data->reopen))
		return;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	err = pthread_create(&data->reopen_thread, &attr, _oss_reopen, data);
	if (err) {
		blog(LOG_ERROR,
		     "Failed to create reopen thread for device '%s'.",
		     data->device);
	}

	pthread_attr_destroy(&attr);
}

void _oss_stop_reopen(struct oss_data *data)
{
	if (os_atomic_load_bool(&data->reopen))
		os_event_signal(data->abort_event);

	if (data->reopen_thread) {
		pthread_join(data->reopen_thread, NULL);
		data->reopen_thread = 0;
	}

	os_event_reset(data->abort_event);
}

void *_oss_listen(void *attr)
{
	struct oss_data *data = attr;
	struct obs_source_audio out;

	blog(LOG_DEBUG, "Capture thread started.");

	out.data[0] = data->buffer;
	out.format = _oss_to_obs_audio_format(data->format);
	out.speakers = _oss_channels_to_obs_speakers(data->channels);
	out.samples_per_sec = data->rate;

	os_atomic_set_bool(&data->listen, true);

	do {
//		snd_pcm_sframes_t frames = snd_pcm_readi(
//			data->handle, data->buffer, data->period_size);
		ssize_t ret;
		ret = read(data->fd, data->buffer, 16384);
		ssize_t frames = ret / 4;

		if (!os_atomic_load_bool(&data->listen))
			break;

//		if (frames <= 0) {
//			frames = snd_pcm_recover(data->handle, frames, 0);
//			if (frames <= 0) {
//				snd_pcm_wait(data->handle, 100);
//				continue;
//			}
//		}
//
		out.frames = frames;
		out.timestamp = os_gettime_ns() -
                	        ((frames * NSEC_PER_SEC) / data->rate);

		if (!data->first_ts)
			data->first_ts = out.timestamp + STARTUP_TIMEOUT_NS;

		if (out.timestamp > data->first_ts)
			obs_source_output_audio(data->source, &out);
	} while (os_atomic_load_bool(&data->listen));

	blog(LOG_DEBUG, "Capture thread is about to exit.");

	pthread_exit(NULL);
	return NULL;
}

void *_oss_reopen(void *attr)
{
	struct oss_data *data = attr;
	unsigned long timeout = REOPEN_TIMEOUT;

	blog(LOG_DEBUG, "Reopen thread started.");

	os_atomic_set_bool(&data->reopen, true);

	while (os_event_timedwait(data->abort_event, timeout) == ETIMEDOUT) {
		if (_oss_open(data))
			break;

		if (timeout < (REOPEN_TIMEOUT * 5))
			timeout += REOPEN_TIMEOUT;
	}

	os_atomic_set_bool(&data->reopen, false);

	blog(LOG_DEBUG, "Reopen thread is about to exit.");

	pthread_exit(NULL);
	return NULL;
}

enum audio_format _oss_to_obs_audio_format(int format)
{
	switch (format) {
	case AFMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AFMT_S16_NE:
		return AUDIO_FORMAT_16BIT;
	case AFMT_S32_NE:
		return AUDIO_FORMAT_32BIT;
//	case SND_PCM_FORMAT_FLOAT_LE:
//		return AUDIO_FORMAT_FLOAT;
	default:
		break;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

enum speaker_layout _oss_channels_to_obs_speakers(unsigned int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	}

	return SPEAKERS_UNKNOWN;
}
