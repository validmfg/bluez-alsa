/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2018 Thierry Bultel (thierry.bultel@iot.bzh)
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <bluealsa.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

static unsigned int verbose = 0;

static const char *ba_interface = "hci0";
static bluezalsa_type ba_type = BA_A2DP;
static bool terminate = false;

static void sig_handle(int sig) {
	terminate = true;
}

int main(int argc,char * argv[]) {
	int opt;
	const char *opts = "hvi:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "hci", required_argument, NULL, 'i' },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ 0, 0, 0, 0 },
	};

	signal(SIGTERM, sig_handle);
	signal(SIGINT, sig_handle);

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <BT-ADDR>...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -v, --verbose\t\tmake output more verbose\n"
					"  -i, --hci=hciX\tHCI device to use\n"
					"  --profile-a2dp\tuse A2DP profile\n"
					"  --profile-sco\t\tuse SCO profile\n"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'v' /* --verbose */ :
			verbose++;
			break;

		case 'i' /* --hci */ :
			ba_interface = optarg;
			break;

		case 1 /* --profile-a2dp */ :
			ba_type = BA_A2DP;
			break;
		case 2 /* --profile-sco */ :
			ba_type = BA_SCO;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind == argc)
		goto usage;

	bdaddr_t ba_addr;

	if (str2ba(argv[optind], &ba_addr) != 0) {
		printf("Malformed given BT address\n");
		goto failed;
	}

	printf("Opening bluetooth interface...\n");

	bluezalsa_handle_t * h = bluezalsa_open(ba_interface);
	if (h == NULL) {
		printf("Failed to open '%s' interface\n", ba_interface);
		goto failed;
	}

	bluezalsa_set_device(h, argv[optind], BA_A2DP);

	size_t frame_size = 2*16;
	size_t buffsize=1024*frame_size;

	int cpt = 0;

	for (;;) {
		char buff[buffsize];
		snd_pcm_sframes_t frames;
		if (terminate)
			break;

		frames = bluezalsa_readi(h, buff, buffsize);
		printf("Main Read %d: %llu frames\n", cpt++, frames);
	}

	printf("Closing bluetooth interface...\n");
	bluezalsa_close(h);

failed:
	printf("Bye\n");
	return 0;
}
