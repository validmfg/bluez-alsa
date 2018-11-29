/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa.h"

#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "../shared/ctl-client.h"

#ifndef ARRAYSIZE
#define ARRAYSIZE(array) sizeof(array)/sizeof(*array)
#endif

/** TODO debug levels **/
#define log printf
#define error printf
#ifdef DEBUG
#define debug printf
#else
#define debug
#endif /* DEBUG */

#define LIBNAME	"bluezalsa-lib"

struct bluezalsa_handle {
	int ba_fd;	/* one per interface */
	int event_fd; /* interface events */
	int snd_fd; /* ports the sound */
	int client_event_fd; /* to notify client that snd_fd has changed */
	char * interface;
	bdaddr_t * addr;
	bluezalsa_type type;
	struct ba_msg_transport transport;
	pthread_t monitor;
	pthread_mutex_t transport_mutex;
};


#define EVENT_CASE(event,e,s) ({ \
    if ((event->mask & e) == (e)) { strcat(s,#e);strcat(s,"|"); \
}})


static void event2string(struct ba_msg_event * event, char * str) {
	str[0] = '\0';
	EVENT_CASE(event, BA_EVENT_TRANSPORT_ADDED, str);
	EVENT_CASE(event, BA_EVENT_TRANSPORT_CHANGED, str);
	EVENT_CASE(event, BA_EVENT_TRANSPORT_REMOVED, str);
	EVENT_CASE(event, BA_EVENT_UPDATE_BATTERY, str);
	EVENT_CASE(event, BA_EVENT_UPDATE_VOLUME, str);
}

static void delete_handle(bluezalsa_handle_t * h) {

	if (!h)
		goto done;

	pthread_mutex_lock(&h->transport_mutex);

	if (h->ba_fd)
		close(h->ba_fd);
	if (h->event_fd)
		close(h->event_fd);
	if (h->client_event_fd)
		close(h->client_event_fd);

	free(h->addr);

	pthread_mutex_unlock(&h->transport_mutex);

	free(h->interface);
	free(h);
done:
	return;
}

static int device_detach(bluezalsa_handle_t * h) {
	int ret = -1;
	char addr[32];

	debug("%s ...\n", __func__);

	if (h == NULL) {
		error("%s: wrong handle\n", __func__);
		goto failed;
	}

	if (h->addr == NULL) {
		error("%s: no address\n", __func__);
		goto done;
	}

	if (h->snd_fd == -1) {
		debug("%s: no snd_fd\n", __func__);
		goto done;
	}

	bluealsa_close_transport(h->ba_fd, &h->transport);

	close(h->snd_fd);
	h->snd_fd = -1;

done:
	ret = 0;
failed:
	return ret;
}

static int update_device_attach_state(bluezalsa_handle_t * h) {
	int ret = -1;
	struct ba_msg_transport *transports = NULL;

	if (!h) {
		error("%s: wrong handle\n", __func__);
		goto failed;
	}

	pthread_mutex_lock(&h->transport_mutex);

	if (h->addr == NULL) {
		/* No monitored device ... */
		error("%s: no address set yet\n", __func__);
		goto failed_locked;
	}

	if (h->ba_fd == -1) {
		error("%s: no connection with bluezalsa daemon\n", __func__);
		goto failed_locked;
	}

	debug("Fetching available transports\n");
	if ((ret = bluealsa_get_transports(h->ba_fd, &transports)) == -1) {
		error("Couldn't get transports: %s", strerror(errno));
		goto failed_locked;
	}

	int nb_transports = ret;
	bool matched = false;
	int idx;

	debug("Fetched %d transports\n", nb_transports);

	/* Iterate other available transport and find a match */
	for (idx = 0; idx < nb_transports; idx++) {
		struct ba_msg_transport *transport = &transports[idx];

#ifdef DEBUG
		char taddr[32], myaddr[32];
		ba2str(&transport->addr, taddr);
		ba2str(h->addr, myaddr);
		debug("Check transport: %s, type %x, stream %x\n",
			   taddr, transport->type, transport->stream);
#endif /* DEBUG */

		/* filter available transports by BT address,
		 * transport type and stream direction */

		if (transport->type != h->type) {
			continue;
		}
		if (transport->stream != BA_PCM_STREAM_CAPTURE &&
			transport->stream != BA_PCM_STREAM_DUPLEX) {
			continue;
		}

		if (bacmp(h->addr, &transports->addr) == 0) {
			debug("Match !\n");
			matched = true;
			break;
		}
	}

	/* If not in the list, close the existing opened transport */
	if (!matched) {
		device_detach(h);
		goto failed_locked;
	}

	if (h->snd_fd != -1) {
		/* We may already be attached. Namely bluezalsa daemon sends spurious events */
		debug("%s: already attached\n", __func__);
		goto done;
	}

	/* Open a transport now */

	struct ba_msg_transport * transport = &transports[idx];

	transport->stream = BA_PCM_STREAM_CAPTURE;
	if ((h->snd_fd = bluealsa_open_transport(h->ba_fd, transport)) == -1) {
		error("Couldn't open PCM FIFO: %s\n", strerror(errno));
		goto failed;
	}

	memcpy(&h->transport, transport, sizeof(struct ba_msg_transport));
	bacpy(&h->transport.addr, h->addr);

	uint64_t u;
	write(h->client_event_fd, &u, sizeof(u));

done:
	ret = 0;

failed_locked:
	pthread_mutex_unlock(&h->transport_mutex);
	free(transports);
failed:
	return ret;
}

static void * monitor_worker_routine(void *arg) {
	bluezalsa_handle_t * h = (bluezalsa_handle_t*) arg;
	for (;;) {
		struct ba_msg_event event;

		int ret;

		struct pollfd pfds[] =  {{ h->event_fd, POLLIN, 0 }};
		if (poll(pfds, ARRAYSIZE(pfds), -1) == -1 && errno == EINTR)
			continue;

		while ((ret = recv(h->event_fd, &event, sizeof(event), MSG_DONTWAIT)) == -1 && errno == EINTR)
			continue;
		if (ret != sizeof(event)) {
			error("Couldn't read event: %s", strerror(ret == -1 ? errno : EBADMSG));
			goto failed;
		}

		char event_string[256];
		event2string(&event, event_string);

		debug("Event on interface %s:%s\n", h->interface, event_string);
		update_device_attach_state(h);
	}
failed:
	return NULL;
}



bluezalsa_handle_t  * bluezalsa_open(const char * interface) {
	bluezalsa_handle_t * h =  NULL;
	int ret;

	 h = (bluezalsa_handle_t*) malloc(sizeof(bluezalsa_handle_t));
	 if (h==NULL) {
		 error("%s,%s: Insufficient memory\n", LIBNAME, __func__);
		 goto failed;
	 }

	 memset(h, 0, sizeof(bluezalsa_handle_t));
	 h->snd_fd = -1;

	 pthread_mutexattr_t attr;
	 pthread_mutexattr_init(&attr);
	 pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);

	 if (pthread_mutex_init(&h->transport_mutex, &attr) != 0) {
		 error("%s: unable to create the transport mutex");
		 goto failed;
	 }

	 h->client_event_fd = eventfd(0, EFD_NONBLOCK);
	 if (h->client_event_fd == -1) {
		 error("Unable to open event fd for client\n");
		 goto failed;
	 }

	 if ((h->ba_fd    = bluealsa_open(interface)) == -1 ||
		 (h->event_fd = bluealsa_open(interface)) == -1) {
		 log("%s,%s: No such interface '%s'\n", LIBNAME, __func__, interface);
		 goto failed;
	 }

	 h->interface = strdup(interface);
	 if (!h->interface) {
		 log("%s,%s: Insufficient memory\n", LIBNAME, __func__);
		 goto failed;
	 }

	 if (bluealsa_subscribe(h->event_fd, BA_EVENT_TRANSPORT_ADDED | BA_EVENT_TRANSPORT_REMOVED) == -1) {
		 log("%s:%s subscription failed: %s", LIBNAME, __func__, strerror(errno));
		 goto failed;
	 }

	 if ((ret = pthread_create(&h->monitor, NULL, monitor_worker_routine, h)) != 0) {
		 log("%s,%s", LIBNAME,__func__);
		 goto failed;
	 }

	 goto done;

failed:

	delete_handle(h);
	return NULL;

done:
	return h;
}

void bluezalsa_close(bluezalsa_handle_t* h) {
	if (!h) {
		error("%s: bad handle\n", __func__);
		goto failed;
	}
	pthread_cancel(h->monitor);
	pthread_join(h->monitor, NULL);

	delete_handle(h);

failed:
	return;
}

int	bluezalsa_set_device(bluezalsa_handle_t * h, const char * addr, bluezalsa_type type) {
	int ret = -1;
	if (!h) {
		error("%s: bad handle\n", __func__);
		goto failed;
	}

	if (type != BA_A2DP &&
		type != BA_SCO) {
		error("Unsupported BT transport type %d\n", type);
		goto failed;
	}

	pthread_mutex_lock(&h->transport_mutex);
	if (h->addr == NULL) {
		goto new_adress;
	}

	free(h->addr);
	h->addr = NULL;
	bluealsa_close_transport(h->ba_fd, &h->transport);
	h->snd_fd = -1;

new_adress:
	if (addr == NULL)
		goto done;

	h->addr = (bdaddr_t*) malloc(sizeof(bdaddr_t));
	if (h->addr == NULL) {
		log("Insufficient memory\n");
		goto failed_locked;
	}

	if (str2ba(addr, h->addr) != 0) {
		error("Malformed BT address: %s\n", addr);
		goto failed_locked;
	}

done:
	h->type = type;
	log("%s: device set to %s\n", __func__, addr);

	pthread_mutex_unlock(&h->transport_mutex);

	if (addr)
		update_device_attach_state(h);

	ret = 0;
	return ret;

failed_locked:
	free(h->addr);
	h->addr = NULL;
	pthread_mutex_unlock(&h->transport_mutex);
failed:
	return ret;
}


/* This function is a blocking call, unless there is a connected device sending data */

snd_pcm_sframes_t 	bluezalsa_readi (bluezalsa_handle_t * h, void *buffer, snd_pcm_uframes_t size) {
	snd_pcm_sframes_t ret = 0;
	int timeout = -1;
	int res;

	if (!h) {
		log("%s: bad handle\n", __func__);
		goto failed;
	}

	struct pollfd pfds[] = {
		{ h->snd_fd, POLLIN, 0},
		{ h->client_event_fd, POLLIN, 0}
	};

again:

	pfds[0].fd = h->snd_fd;
	res = poll(pfds, ARRAYSIZE(pfds), timeout);

	if (res == -1) {
		log("%s: poll failed (%s)\n", __func__, strerror(errno));
		goto failed;
	}

	if (res == 0) {
		/* timeout. cannot happen */
		goto again;
	}

	if (pfds[0].revents & POLLHUP) {
		device_detach(h);
		goto again;
	}

	/* Check if there has been a sound fd change */
	if (pfds[1].revents & POLLIN) {
		uint64_t u;

		while ((ret = read(h->client_event_fd, &u, sizeof(uint64_t))) == -1 &&
				errno == EINTR)
			continue;
		if (ret != sizeof(u)) {
			error("Couldn't read event: %s", strerror(ret == -1 ? errno : EBADMSG));
			goto failed;
		}
	}

	if (!(pfds[0].revents & POLLIN)) {
		debug("%s: nothing to read\n", __func__);
		goto again;
	}

	if ((res = read(h->snd_fd, buffer, size * sizeof(int16_t))) == -1) {
		error("PCM FIFO read error: %s\n", strerror(errno));
		/* An error on fd here is likely due to a broken transport.
		 * To poll again, without returning, in this way the caller will not
		 * be aware of that */

		goto again;
	}
	ret = res;
failed:
	return ret;
}

snd_pcm_sframes_t 	bluezalsa_writei (bluezalsa_handle_t *h, const void *buffer, snd_pcm_uframes_t size) {
	snd_pcm_sframes_t ret = 0;
	/* TODO */
	return ret;
}
