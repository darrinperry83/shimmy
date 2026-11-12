// coreaudio_host.c — play the shim's ALSA playback ring buffer out the Mac's default output.
//
// A single AudioUnit (kAudioUnitSubType_DefaultOutput) pulls interleaved float32 frames from
// snd_alsa.c (snd_playback_pull) on its render thread and hands them to CoreAudio. The guest's
// PipeWire writes into the ALSA mmap ring buffer on its own threads; this callback is the
// "hardware" that consumes those frames at the real sample rate, which is what advances hw_ptr
// and lets the guest's poll() pace itself to the clock.
//
// The client (input scope) format is interleaved float32, so CoreAudio performs the float ->
// device conversion and any device sample-rate conversion. All API here is the pure-C AudioUnit
// API in AudioToolbox, so this links into shim_cli (headless) as well as shim_gui.

#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "snd_alsa.h"

static AudioUnit g_unit = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started = 0;
static unsigned g_rate = 0;
static unsigned g_channels = 0;

static OSStatus render_cb(void* inRefCon,
                          AudioUnitRenderActionFlags* ioActionFlags,
                          const AudioTimeStamp* inTimeStamp,
                          UInt32 inBusNumber,
                          UInt32 inNumberFrames,
                          AudioBufferList* ioData) {
  (void)inRefCon; (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;
  unsigned ch = g_channels ? g_channels : 2;
  float* out = (float*)ioData->mBuffers[0].mData;
  unsigned got = snd_playback_pull(out, inNumberFrames);
  if (got < inNumberFrames) {
    size_t remain = (size_t)(inNumberFrames - got) * ch * sizeof(float);
    memset(out + (size_t)got * ch, 0, remain);
  }
  return noErr;
}

static void ca_setup(unsigned rate, unsigned channels) {
  AudioComponentDescription desc = {0};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_DefaultOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (!comp) { fprintf(stderr, "[ca] no default output AudioComponent\n"); return; }

  AudioUnit unit = NULL;
  if (AudioComponentInstanceNew(comp, &unit) != noErr || !unit) {
    fprintf(stderr, "[ca] AudioComponentInstanceNew failed\n");
    return;
  }

  AudioStreamBasicDescription fmt = {0};
  fmt.mSampleRate       = (double)rate;
  fmt.mFormatID         = kAudioFormatLinearPCM;
  fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagsNativeEndian;
  fmt.mBitsPerChannel   = 32;
  fmt.mChannelsPerFrame = channels;
  fmt.mBytesPerFrame    = 4 * channels;
  fmt.mFramesPerPacket  = 1;
  fmt.mBytesPerPacket   = 4 * channels;

  OSStatus s = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
  if (s != noErr) fprintf(stderr, "[ca] set stream format: %d\n", (int)s);

  AURenderCallbackStruct cb = { render_cb, NULL };
  s = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &cb, sizeof cb);
  if (s != noErr) fprintf(stderr, "[ca] set render callback: %d\n", (int)s);

  if (AudioUnitInitialize(unit) != noErr) {
    fprintf(stderr, "[ca] AudioUnitInitialize failed\n");
    AudioComponentInstanceDispose(unit);
    return;
  }
  g_unit = unit;
  g_rate = rate;
  g_channels = channels;
  fprintf(stderr, "[ca] AudioUnit ready: %u Hz, %u ch\n", rate, channels);
}

void ca_playback_start(unsigned rate, unsigned channels) {
  if (!snd_enabled()) return;
  if (!rate || !channels) return;
  pthread_mutex_lock(&g_lock);
  if (g_unit && (rate != g_rate || channels != g_channels)) {
    if (g_started) { AudioOutputUnitStop(g_unit); g_started = 0; }
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    g_unit = NULL;
  }
  if (!g_unit) ca_setup(rate, channels);
  if (g_unit && !g_started) {
    if (AudioOutputUnitStart(g_unit) == noErr) {
      g_started = 1;
      fprintf(stderr, "[ca] AudioOutputUnitStart: running (CoreAudio sink live)\n");
    } else {
      fprintf(stderr, "[ca] AudioOutputUnitStart failed\n");
    }
  }
  pthread_mutex_unlock(&g_lock);
}

void ca_playback_stop(void) {
  pthread_mutex_lock(&g_lock);
  if (g_unit && g_started) {
    AudioOutputUnitStop(g_unit);
    g_started = 0;
    fprintf(stderr, "[ca] AudioOutputUnitStop\n");
  }
  pthread_mutex_unlock(&g_lock);
}
