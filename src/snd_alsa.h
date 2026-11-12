// snd_alsa.h — a synthetic ALSA card (/dev/snd/controlC0 + pcmC0D0p/c) for the guest.
//
// The Arch guest ships PipeWire + pipewire-pulse + wireplumber, which enumerate an ALSA
// card and drive it through libasound. There is no ALSA/DRM audio hardware on macOS, so the
// shim presents a virtual char device (the same shape as drmasahi.c presents /dev/dri) and
// answers the SNDRV_CTL_IOCTL_* / SNDRV_PCM_IOCTL_* ioctls itself. PCM written by the guest
// into the mmap'd ring buffer is pulled by CoreAudio (coreaudio_host.c) and played out the
// Mac's default output device.
#ifndef SND_ALSA_H
#define SND_ALSA_H
#include <stdint.h>
#include <stddef.h>

int  snd_enabled(void);                     // RDSND=1
int  snd_is_node(const char* path);         // /dev/snd/controlC0, /dev/snd/pcmC0D0p, /dev/snd/pcmC0D0c
void snd_register_fd(int fd);
void snd_assign_fd(int fd, int stream_dir); // 0=playback, 1=capture (for pcm nodes)
void snd_dup_fd(int oldfd, int newfd);
void snd_forget_fd(int fd);
int  snd_owns_fd(int fd);
long snd_ioctl(int fd, unsigned long req, unsigned long arg);
void* snd_mmap(int fd, long off);           // ALSA mmap offset -> backing, NULL if none
void snd_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path);
int  snd_sysfs_kind(const char* path);      // synthetic /sys/class/sound/... and /proc/asound handled elsewhere
const char* snd_proc_content(const char* path); // /proc/asound/cards, /proc/asound/pcm, /proc/asound/version
int  snd_poll_revents(int fd, short events);// Linux revents for poll()/ppoll() on a PCM fd (non-blocking)

// ---- CoreAudio bridge (called from coreaudio_host.c) -------------------------------
// Return the negotiated playback parameters, or 0 if not yet configured.
unsigned snd_playback_rate(void);
unsigned snd_playback_channels(void);
// Pull up to `nframes` frames of interleaved float32 into `dst` (frames*channels floats),
// consuming them from the ALSA ring buffer. Returns the number of frames pulled (may be less
// than requested on underrun; caller fills the remainder with silence).
unsigned snd_playback_pull(float* dst, unsigned nframes);
// Signal the CoreAudio host to (re)start/stop when the stream state changes.
void snd_playback_notify(void);

// ---- implemented by coreaudio_host.c (called from snd_alsa.c) --------------------------
void ca_playback_start(unsigned rate, unsigned channels);
void ca_playback_stop(void);

#endif
