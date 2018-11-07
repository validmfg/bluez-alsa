/*
 * bluealsa-proxy-pcm.c
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "shared/ctl-client.h"
#include "shared/ctl-proto.h"
#include "shared/log.h"
#include "shared/rt.h"

#undef debug
#define debug printf

#define INTERFACE_STR_MAXLEN	256
#define BDADDR_STR_LEN			18
#define PROFILE_STR_MAXLEN		16

struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* bluealsa socket */
	int fd;

	/* event file descriptor */
	int event_fd;

	/* requested transport */
	struct msg_transport * transport;
	size_t pcm_buffer_size;

	int pcm_fd;

	/* virtual hardware - ring buffer */
	snd_pcm_uframes_t io_ptr;
	pthread_t io_thread;
	bool io_started;

	/* communication and encoding/decoding delay */
	snd_pcm_sframes_t delay;
	/* user provided extra delay component */
	snd_pcm_sframes_t delay_ex;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;

	/* In order to see whether the PCM has reached under-run (or over-run), we
	 * have to know the exact position of the hardware and software pointers.
	 * To do so, we could use the HW pointer provided by the IO plug structure.
	 * This pointer is updated by the snd_pcm_hwsync() function, which is not
	 * thread-safe (total disaster is guaranteed). Since we can not call this
	 * function, we are going to use our own HW pointer, which can be updated
	 * safely in our IO thread. */
	snd_pcm_uframes_t io_hw_boundary;
	snd_pcm_uframes_t io_hw_ptr;

	char interface[INTERFACE_STR_MAXLEN];
	bdaddr_t addr;
	char profile[PROFILE_STR_MAXLEN];
	enum pcm_type type;
	enum pcm_stream stream;

};

typedef struct bluealsa_pcm bluealsa_pcm_t;

static bluealsa_pcm_t * the_pcm = NULL;

static int close_bluez_connection();
static int open_bluez_connection();


/**
 * Helper function for closing PCM transport. */
static int close_transport(struct bluealsa_pcm *pcm) {

	debug("%s ...\n", __func__);
	if (pcm->transport == NULL)
		return 0;

	int rv = bluealsa_close_transport(pcm->fd, pcm->transport);
	int err = errno;

	close(pcm->pcm_fd);
	pcm->pcm_fd = -1;
	errno = err;
	return rv;
}

/**
 * IO thread, which facilitates ring buffer. */
static void *io_thread(void *arg) {
	snd_pcm_ioplug_t *io = (snd_pcm_ioplug_t *)arg;

	struct bluealsa_pcm *pcm = io->private_data;

	sigset_t sigset;
	sigemptyset(&sigset);

	/* Block signal, which will be used for pause/resume actions. */
	sigaddset(&sigset, SIGIO);
	/* Block SIGPIPE, so we could receive EPIPE while writing to the pipe
	 * whose reading end has been closed. This will allow clean playback
	 * termination. */
	sigaddset(&sigset, SIGPIPE);

	if ((errno = pthread_sigmask(SIG_BLOCK, &sigset, NULL)) != 0) {
		SNDERR("Thread signal mask error: %s", strerror(errno));
		goto final;
	}

wait_pcm_fd:

	debug("PLUGIN io-thread: wait for pcm_fd\n");

	/* In the capture mode, the PCM FIFO is opened in the non-blocking mode.
	 * So right now, we have to synchronize write and read sides, otherwise
	 * reading might return 0, which will be incorrectly recognized as FIFO
	 * close signal, but in fact it means, that it was not opened yet. */
	if (io->stream == SND_PCM_STREAM_CAPTURE) {

		/* Is the data fd ready ? */
		if (pcm->pcm_fd == -1) {
			usleep(100*1000);	// TODO This should be replaced by a semaphore
			goto wait_pcm_fd;
		}

		/* check both fd status */

		struct pollfd pfds[2] = {
				{ pcm->pcm_fd, POLLIN, 0 },
				{ pcm->fd, 	POLLIN|POLLPRI, 0 }};

		int ret = poll(pfds, 2, -1);
		if (ret == -1) {
			SNDERR("PCM FIFO poll error: %s", strerror(errno));
			goto final;
		}

		short int evt0 = pfds[0].revents;
		short int evt1 = pfds[1].revents;

		if (evt1 & POLLHUP) {
			debug("Server closed the connection\n");
			close_bluez_connection();
			open_bluez_connection();
			usleep(100*1000); /* avoid spinning too fast /*/
			goto wait_pcm_fd;
		}

		if (evt0 & POLLHUP) {
			debug("Remote device disconnected\n");
			close_bluez_connection();
			open_bluez_connection();
			usleep(100*1000);
			goto wait_pcm_fd;
		}
	}

	const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);

	struct asrsync asrs;
	asrsync_init(asrs, io->rate);

	for (;;) {

		int tmp;
		switch (io->state) {
		case SND_PCM_STATE_RUNNING:
		case SND_PCM_STATE_DRAINING:
			break;
		case SND_PCM_STATE_DISCONNECTED:
			debug("PLUGIN: DISCONNECTED\n");
			goto final;
		default:
			sigwait(&sigset, &tmp);
			asrsync_init(asrs, io->rate);
		}

		snd_pcm_uframes_t io_ptr = pcm->io_ptr;
		snd_pcm_uframes_t io_buffer_size = io->buffer_size;
		snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;
		snd_pcm_uframes_t io_hw_boundary = pcm->io_hw_boundary;
		snd_pcm_uframes_t frames = io->period_size;
		char *buffer = areas->addr + (areas->first + areas->step * io_ptr) / 8;
		char *head = buffer;
		ssize_t ret = 0;
		size_t len;

		/* If the leftover in the buffer is less than a whole period sizes,
		 * adjust the number of frames which should be transfered. It has
		 * turned out, that the buffer might contain fractional number of
		 * periods - it could be an ALSA bug, though, it has to be handled. */
		if (io_buffer_size - io_ptr < frames)
			frames = io_buffer_size - io_ptr;

		/* IO operation size in bytes */
		len = frames * pcm->frame_size;
		io_ptr += frames;

		if (io_ptr >= io_buffer_size)
			io_ptr -= io_buffer_size;

		io_hw_ptr += frames;
		if (io_hw_ptr >= io_hw_boundary)
			io_hw_ptr -= io_hw_boundary;

		if (io->stream == SND_PCM_STREAM_CAPTURE) {

			/* Read the whole period "atomically". This will assure, that frames
			 * are not fragmented, so the pointer can be correctly updated. */
			while (len != 0 && (ret = read(pcm->pcm_fd, head, len)) != 0) {
				if (ret == -1) {
					if (errno == EINTR)
						continue;

					SNDERR("PCM FIFO read error: %s", strerror(errno));
					goto final;
				}
				head += ret;
				len -= ret;
			}

			/* something went wrong. this can be a server,
			 * or device disconnection, or a device change request
			 * from the client application */

			if (ret == 0)
				goto wait_pcm_fd;

		}
		else {

			/* check for under-run and act accordingly */
			if (io_hw_ptr > io->appl_ptr) {
				io->state = SND_PCM_STATE_XRUN;
				io_ptr = -1;
				goto sync;
			}

			/* Perform atomic write - see the explanation above. */
			do {
				if ((ret = write(pcm->pcm_fd, head, len)) == -1) {
					if (errno == EINTR)
						continue;
					SNDERR("PCM FIFO write error: %s", strerror(errno));
					goto final;
				}
				head += ret;
				len -= ret;
			}
			while (len != 0);

			/* synchronize playback time */
			asrsync_sync(&asrs, frames);
		}

sync:
		pcm->io_ptr = io_ptr;
		pcm->io_hw_ptr = io_hw_ptr;

		eventfd_write(pcm->event_fd, 1);
	}

final:

	debug("PLUGIN: io_thread exiting\n");

	close_transport(pcm);
	eventfd_write(pcm->event_fd, 0xDEAD0000);

	return NULL;
}

static int bluealsa_proxy_start(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	debug("%s ...\n", __func__);

	/* If the IO thread is already started, skip thread creation. Otherwise,
	 * we might end up with a bunch of IO threads reading or writing to the
	 * same FIFO simultaneously. Instead, just send resume signal. */
	if (pcm->io_started) {
		io->state = SND_PCM_STATE_RUNNING;
		pthread_kill(pcm->io_thread, SIGIO);
		return 0;
	}

	/* initialize delay calculation */
	pcm->delay = 0;

	if (pcm->transport && bluealsa_pause_transport(pcm->fd, pcm->transport, false) == -1) {
		debug("Couldn't start PCM: %s\n", strerror(errno));
		return -errno;
	}

	/* State has to be updated before the IO thread is created - if the state
	 * does not indicate "running", the IO thread will be suspended until the
	 * "resume" signal is delivered. This requirement is only (?) theoretical,
	 * anyhow forewarned is forearmed. */
	snd_pcm_state_t prev_state = io->state;
	io->state = SND_PCM_STATE_RUNNING;

	pcm->io_started = true;

	return 0;
}

static int bluealsa_proxy_stop(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	debug("Stopping");
	if (pcm->io_started) {
		pcm->io_started = false;
		pthread_cancel(pcm->io_thread);
		pthread_join(pcm->io_thread, NULL);
	}
	return 0;
}

static snd_pcm_sframes_t bluealsa_proxy_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	if (pcm->pcm_fd == -1)
		return -ENODEV;

	return pcm->io_ptr;
}

static int bluealsa_proxy_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	debug("Closing plugin\n");
	close(pcm->fd);
	close(pcm->event_fd);
	free(pcm);

	the_pcm = NULL;

	return 0;
}

static int bluealsa_proxy_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	/* Indicate that our PCM is ready for writing, even though is is not 100%
	 * true - IO thread is not running yet. Some weird implementations might
	 * require PCM to be writable before the snd_pcm_start() call. */
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		eventfd_write(pcm->event_fd, 1);

	if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK) {
		/* By default, the size of the pipe buffer is set to a too large value for
		 * our purpose. On modern Linux system it is 65536 bytes. Large buffer in
		 * the playback mode might contribute to an unnecessary audio delay. Since
		 * it is possible to modify the size of this buffer we will set is to some
		 * low value, but big enough to prevent audio tearing. Note, that the size
		 * will be rounded up to the page size (typically 4096 bytes). */
		pcm->pcm_buffer_size = fcntl(pcm->pcm_fd, F_SETPIPE_SZ, 2048);
		debug("FIFO buffer size: %zd", pcm->pcm_buffer_size);
	}

	debug("Selected HW buffer: %zd periods x %zd bytes %c= %zd bytes\n",
			io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
			io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
			io->buffer_size * pcm->frame_size);

	return 0;
}

static int bluealsa_proxy_hw_free(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Freeing HW");

	if (close_transport(pcm) == -1)
		return -errno;
	return 0;
}

static int bluealsa_proxy_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	snd_pcm_sw_params_get_boundary(params, &pcm->io_hw_boundary);
	return 0;
}

static int bluealsa_proxy_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* initialize ring buffer */
	pcm->io_hw_ptr = 0;
	pcm->io_ptr = 0;

	return 0;
}

static int bluealsa_proxy_drain(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (bluealsa_drain_transport(pcm->fd, pcm->transport) == -1)
		return -errno;
	return 0;
}

static int bluealsa_proxy_pause(snd_pcm_ioplug_t *io, int enable) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (bluealsa_pause_transport(pcm->fd, pcm->transport, enable) == -1)
		return -errno;

	if (enable == 0) {
		io->state = SND_PCM_STATE_RUNNING;
		pthread_kill(pcm->io_thread, SIGIO);
	}

	/* Even though PCM transport is paused, our IO thread is still running. If
	 * the implementer relies on the PCM file descriptor readiness, we have to
	 * bump our internal event trigger. Otherwise, client might stuck forever
	 * in the poll/select system call. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static void bluealsa_proxy_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
	struct bluealsa_pcm *pcm = io->private_data;
	char addr[18];

	if (pcm->transport == NULL) {
		snd_output_printf(out, "Bluetooth Proxy: no transport yet\n");
		return;
	}

	ba2str(&pcm->transport->addr, addr);
	snd_output_printf(out, "Bluetooth Proxy device: %s\n", addr);
	snd_output_printf(out, "Bluetooth Proxy profile: %d\n", pcm->transport->type);
	snd_output_printf(out, "Bluetooth Proxy codec: %d\n", pcm->transport->codec);

}

static int bluealsa_proxy_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (pcm->pcm_fd == -1)
		return -ENODEV;

	/* Exact calculation of the PCM delay is very hard, if not impossible. For
	 * the sake of simplicity we will make few assumptions and approximations.
	 * In general, the delay is proportional to the number of bytes queued in
	 * the FIFO buffer, the time required to encode data, Bluetooth transfer
	 * latency and the time required by the device to decode and play audio. */

	static int counter = 0;
	snd_pcm_sframes_t delay = 0;
	unsigned int size;

	/* bytes queued in the PCM ring buffer */
	delay += io->appl_ptr - io->hw_ptr;

	/* bytes queued in the FIFO buffer */
	if (ioctl(pcm->pcm_fd, FIONREAD, &size) != -1)
		delay += size / pcm->frame_size;

	/* On the server side, the delay stat will not be available until the PCM
	 * data transfer is started. Do not make an unnecessary call then. */
	if ((io->state == SND_PCM_STATE_RUNNING || io->state == SND_PCM_STATE_DRAINING)) {

		/* data transfer (communication) and encoding/decoding */
		if (io->stream == SND_PCM_STREAM_PLAYBACK &&
				(pcm->delay == 0 || ++counter % (io->rate / 10) == 0)) {

			unsigned int tmp;
			if ((tmp = bluealsa_get_transport_delay(pcm->fd, pcm->transport)) != -1) {
				pcm->delay = (io->rate / 100) * tmp / 100;
				debug("BlueALSA delay: %.1f ms (%ld frames)", (float)tmp / 10, pcm->delay);
			}

		}

	}

	*delayp = delay + pcm->delay + pcm->delay_ex;
	return 0;
}

static int bluealsa_proxy_poll_descriptors_count(snd_pcm_ioplug_t *io) {
	(void)io;
	return 1;
}

static int bluealsa_proxy_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int space) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (space != 1)
		return -EINVAL;

	pfd[0].fd = pcm->event_fd;
	pfd[0].events = POLLIN;

	return 1;
}

static int bluealsa_proxy_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int nfds, unsigned short *revents) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (nfds != 1) {
		return -EINVAL;
	}

	if (pcm->pcm_fd == -1) {
		return -ENODEV;
	}

	if (pfd[0].revents & POLLIN) {

		eventfd_t event;
		eventfd_read(pcm->event_fd, &event);

		if (event & 0xDEAD0000) {
			goto fail;
		}

		/* If the event was triggered prematurely, wait for another one.
		 * This causes a deadlock if */
		if (!snd_pcm_avail_update(io->pcm)) {
			return *revents = 0;
		}

		/* ALSA expects that the event will match stream direction, e.g.
		 * playback will not start if the event is for reading. */
		*revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

	}
	else
		*revents = 0;

	return 0;

fail:
	*revents = POLLERR | POLLHUP;
	return -ENODEV;
}

static const snd_pcm_ioplug_callback_t bluealsa_proxy_callback = {
	.start = bluealsa_proxy_start,
	.stop = bluealsa_proxy_stop,
	.pointer = bluealsa_proxy_pointer,
	.close = bluealsa_proxy_close,
	.hw_params = bluealsa_proxy_hw_params,
	.hw_free = bluealsa_proxy_hw_free,
	.sw_params = bluealsa_proxy_sw_params,
	.prepare = bluealsa_proxy_prepare,
	.drain = bluealsa_proxy_drain,
	.pause = bluealsa_proxy_pause,
	.dump = bluealsa_proxy_dump,
	.delay = bluealsa_proxy_delay,
	.poll_descriptors_count = bluealsa_proxy_poll_descriptors_count,
	.poll_descriptors = bluealsa_proxy_poll_descriptors,
	.poll_revents = bluealsa_proxy_poll_revents,
};

static enum pcm_type bluealsa_proxy_parse_profile(const char *profile) {

	if (profile == NULL)
		return PCM_TYPE_NULL;

	if (strcasecmp(profile, "a2dp") == 0)
		return PCM_TYPE_A2DP;
	else if (strcasecmp(profile, "sco") == 0)
		return PCM_TYPE_SCO;

	return PCM_TYPE_NULL;
}

/* This must be called when a transport is available */

static int bluealsa_proxy_set_hw_constraint(struct bluealsa_pcm *pcm) {
	snd_pcm_ioplug_t *io = &pcm->io;

	static const snd_pcm_access_t accesses[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_S16_LE,
	};

	int err;

	debug("Setting constraints\n");

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					sizeof(accesses) / sizeof(*accesses), accesses)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					sizeof(formats) / sizeof(*formats), formats)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					2, 1024)) < 0)
		return err;

	/* In order to prevent audio tearing and minimize CPU utilization, we're
	 * going to setup buffer size constraints. These limits are derived from
	 * the transport sampling rate and the number of channels, so the buffer
	 * "time" size will be constant. The minimal period size and buffer size
	 * are respectively 10 ms and 200 ms. Upper limits are not constraint. */
	unsigned int min_p = pcm->transport->sampling * 10 / 1000 * pcm->transport->channels * 2;
	unsigned int min_b = pcm->transport->sampling * 200 / 1000 * pcm->transport->channels * 2;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					min_p, 1024 * 16)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					min_b, 1024 * 1024 * 16)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					pcm->transport->channels, pcm->transport->channels)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					pcm->transport->sampling, pcm->transport->sampling)) < 0)
		return err;

	return 0;
}


SND_PCM_PLUGIN_DEFINE_FUNC(bluealsa_proxy) {
	(void)root;

	struct bluealsa_pcm *pcm;
	long delay = 0;
	int ret;

	if ((pcm = calloc(1, sizeof(*pcm))) == NULL)
		return -ENOMEM;

	pcm->fd = -1;
	pcm->event_fd = -1;
	pcm->pcm_fd = -1;
	pcm->delay_ex = delay;

	if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		ret = -errno;
		goto fail;
	}

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	pcm->io.mmap_rw = 1;
	pcm->io.callback = &bluealsa_proxy_callback;
	pcm->io.private_data = pcm;
	pcm->transport = NULL;

	enum pcm_stream _stream = stream == SND_PCM_STREAM_PLAYBACK ?
			PCM_STREAM_PLAYBACK : PCM_STREAM_CAPTURE;

	pcm->stream = stream;

	if ((ret = snd_pcm_ioplug_create(&pcm->io, name, stream, mode)) < 0)
		goto fail;

	*pcmp = pcm->io.pcm;

	// Remember PCM
	the_pcm = pcm;

	if ((errno = pthread_create(&pcm->io_thread, NULL, io_thread, &pcm->io)) != 0) {
		debug("Couldn't create IO thread: %s", strerror(errno));
		pcm->io_started = false;

		ret = -errno;
		goto fail;
	}

	pthread_setname_np(pcm->io_thread, "pcm-io");
	return 0;

fail:
	if (pcm->fd != -1)
		close(pcm->fd);
	if (pcm->event_fd != -1)
		close(pcm->event_fd);
	free(pcm);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(bluealsa_proxy);

static int open_bluez_connection() {
	int ret = 0;
	bluealsa_pcm_t * pcm = the_pcm;

	char addr[256];
	ba2str(&pcm->addr, addr);

	debug("%s interface %s addr %s type %d\n", __func__, pcm->interface, addr, pcm->type);

	if ((pcm->fd = bluealsa_open(pcm->interface)) == -1) {
		SNDERR("BlueALSA connection failed: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	if ((pcm->transport = bluealsa_get_transport(pcm->fd, pcm->addr, pcm->type, pcm->stream)) == NULL) {
		SNDERR("Couldn't get BlueALSA transport: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	if ((ret = bluealsa_proxy_set_hw_constraint(pcm)) < 0) {
		snd_pcm_ioplug_delete(&pcm->io);
		goto fail;
	}

	pcm->transport->stream = pcm->stream;

	if ((pcm->pcm_fd = bluealsa_open_transport(pcm->fd, pcm->transport)) == -1) {
		debug("Couldn't open PCM FIFO: %s", strerror(errno));
		return -errno;
	}

	debug("PLUGIN: starting transport\n");

	if (bluealsa_pause_transport(pcm->fd, pcm->transport, false) == -1) {
		debug("Couldn't start PCM: %s", strerror(errno));
		return -errno;
	}

	debug("PLUGIN: connection ready !\n");

	return 0;

fail:
	return ret;
}

static int close_bluez_connection() {
	int ret = 0;
	bluealsa_pcm_t * pcm = the_pcm;
	close_transport(pcm);
	close(pcm->fd);
	return ret;
}

/* This is an exported function, to be loaded by clients via dlsym */

int bluealsa_proxy_set_remote_device(const char * interface, const char * device, const char * profile) {
	int ret  = -1;

	debug("PLUGIN: %s: interface %s device %s profile %s\n", __func__, interface, device, profile);

	if (the_pcm == NULL) {
		SNDERR("No current opened connection to bluezalsa server\n");
		goto failed;
	}

	enum pcm_type type;

	if (device == NULL || str2ba(device, &the_pcm->addr) != 0) {
		SNDERR("Invalid BT device address: %s", device);
		ret = -EINVAL;
		goto failed;
	}

	if ((type = bluealsa_proxy_parse_profile(profile)) == PCM_TYPE_NULL) {
		SNDERR("Invalid BT profile [a2dp, sco]: %s", profile);
		ret = -EINVAL;
		goto failed;
	}

	strncpy(the_pcm->profile, profile, PROFILE_STR_MAXLEN);
	strncpy(the_pcm->interface, interface, INTERFACE_STR_MAXLEN);

	the_pcm->type = type;

	close_bluez_connection();
	ret = open_bluez_connection();

failed:
	return ret;
}

SND_DLSYM_BUILD_VERSION(bluealsa_proxy_set_remote_device, SND_PCM_DLSYM_VERSION);
