# ioplug proxy

The ioplug proxy maintains a consitent state of the opened PCM.

The application can call snd_pcm_open giving "bluealsa_proxy" as a card name.
Only one PCM of this type can be opened by the application, and for now the
hci interface is assumed to be hci0.

To change to another one, just modify this line in 20-blualsa_proxu.conf
defaults.bluealsa.interface "hci0"

One the PCM is opened, which always succeeds regardless of the fact that there is
a bluetooth device connected or not, the application can set it dynamically using
the bluealsa_proxy_set_remote_device function, like in this example code:

```
#define ALSA_BLUEZ_PROXY_LIB "/usr/lib/alsa-lib/libasound_module_pcm_bluealsa_proxy.so"
#define ALSA_BLUEZ_PROXY_SETDEVICE "bluealsa_proxy_set_remote_device"

typedef	int (*bluealsa_set_remote_device_ptr) (const char * interface, const char * device, const char * profile);

static bluealsa_set_remote_device_ptr bluealsa_proxy_set_remote_device = NULL;

void alsa_bluez_init() {
	static bool initialized = false;
	if (initialized)
		goto failed;

	char errbuf[256];
	void * dl = snd_dlopen(ALSA_BLUEZ_PROXY_LIB, RTLD_NOW, errbuf, 256);
	if (!dl) {
		printf("Failed to open bluealsa proxy plugin\n");
		goto failed;
	}

	void * func = snd_dlsym(dl, ALSA_BLUEZ_PROXY_SETDEVICE, SND_DLSYM_VERSION(SND_PCM_DLSYM_VERSION));
	if (!func) {
		printf("Unable to find %s symbol\n", ALSA_BLUEZ_PROXY_SETDEVICE);
		goto failed;
	}

	bluealsa_proxy_set_remote_device = func;
	initialized = true;

failed:
	return;
}

int alsa_bluez_set_device(const char * interface, const char * device, const char * profile) {
	if (!bluealsa_proxy_set_remote_device)
		return -1;

	return bluealsa_proxy_set_remote_device(interface,device,profile);
}

...
alsa_bluez_set_device("hci0","XX.XX.XX.XX.XX", "a2dp");
...

```
