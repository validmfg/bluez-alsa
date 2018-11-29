/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

/* This is the public interface of the library */

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>

typedef struct bluezalsa_handle bluezalsa_handle_t;

/* TODO share this instead of redefining it */
typedef enum {
	BA_TYPE_NULL = 0,
	BA_A2DP,
	BA_SCO
} bluezalsa_type;

extern bluezalsa_handle_t  * bluezalsa_open(const char * interface);
extern void 				 bluezalsa_close(bluezalsa_handle_t* h);

extern int					bluezalsa_set_device(bluezalsa_handle_t * h, const char * addr, bluezalsa_type t);

extern snd_pcm_sframes_t 	bluezalsa_readi (bluezalsa_handle_t * h, void *buffer, snd_pcm_uframes_t size);
extern snd_pcm_sframes_t 	bluezalsa_writei (bluezalsa_handle_t *h, const void *buffer, snd_pcm_uframes_t size);
