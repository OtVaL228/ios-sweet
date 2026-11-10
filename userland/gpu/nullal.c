/*
 * 0193: a silent OpenAL for apps, part of libsweet-glapp.dylib.
 *
 * The port has no audio output, so OpenAL's alcOpenDevice(NULL) returns NULL. Angry Birds Classic throws
 * "Failed to open OpenAL device.", carries on without its game::AudioSystemComponent and then crashes reading it
 * (createStartUpAssets, address 0x10). This interposes the OpenAL calls apps make. While the real device opens,
 * everything goes to OpenAL unchanged. When it does not, the app gets a fake device and context, and the al* calls
 * act on a small model of sources and buffers that plays nothing but keeps time: a playing source runs through its
 * buffers at their real length (bytes / (channels * bytes per sample * frequency)), so AL_BUFFERS_PROCESSED and
 * AL_SOURCE_STATE move the way a streaming or one-shot player expects, without spinning.
 *
 * Interposers are dyld's __DATA,__interpose: they do not apply to this library's own calls, so the real functions
 * are called by name here. Lines go to /var/local/gl.log as "AL[pid name]: ...".
 * MIT, as userland/.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* OpenAL's types and constants by their ABI (OpenAL.framework's headers are deprecated and noisy) */
typedef char ALboolean;
typedef char ALCboolean;
typedef int ALint;
typedef int ALCint;
typedef unsigned int ALuint;
typedef int ALsizei;
typedef int ALenum;
typedef float ALfloat;
typedef void ALvoid;
typedef char ALCchar;
typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;

#define AL_LOOPING 0x1007
#define AL_BUFFER 0x1009
#define AL_GAIN 0x100A
#define AL_SOURCE_STATE 0x1010
#define AL_INITIAL 0x1011
#define AL_PLAYING 0x1012
#define AL_PAUSED 0x1013
#define AL_STOPPED 0x1014
#define AL_BUFFERS_QUEUED 0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_SEC_OFFSET 0x1024
#define AL_SOURCE_TYPE 0x1027
#define AL_STATIC 0x1028
#define AL_STREAMING 0x1029
#define AL_UNDETERMINED 0x1030
#define AL_PITCH 0x1003
#define AL_FORMAT_MONO8 0x1100
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO8 0x1102
#define AL_FORMAT_STEREO16 0x1103

/* the real ones (OpenAL.framework) */
extern ALCdevice *alcOpenDevice(const ALCchar *);
extern ALCboolean alcCloseDevice(ALCdevice *);
extern ALCcontext *alcCreateContext(ALCdevice *, const ALCint *);
extern ALCboolean alcMakeContextCurrent(ALCcontext *);
extern void alcProcessContext(ALCcontext *);
extern void alcSuspendContext(ALCcontext *);
extern void alcDestroyContext(ALCcontext *);
extern void alGenBuffers(ALsizei, ALuint *);
extern void alDeleteBuffers(ALsizei, const ALuint *);
extern void alBufferData(ALuint, ALenum, const ALvoid *, ALsizei, ALsizei);
extern void alGenSources(ALsizei, ALuint *);
extern void alDeleteSources(ALsizei, const ALuint *);
extern void alSourcef(ALuint, ALenum, ALfloat);
extern void alSource3f(ALuint, ALenum, ALfloat, ALfloat, ALfloat);
extern void alSourcei(ALuint, ALenum, ALint);
extern void alGetSourcef(ALuint, ALenum, ALfloat *);
extern void alGetSourcei(ALuint, ALenum, ALint *);
extern void alSourcePlay(ALuint);
extern void alSourceStop(ALuint);
extern void alSourceQueueBuffers(ALuint, ALsizei, const ALuint *);
extern void alSourceUnqueueBuffers(ALuint, ALsizei, ALuint *);

static pthread_mutex_t nal_lock = PTHREAD_MUTEX_INITIALIZER;
static int nal_fake;                    /* the real device did not open: every al* call is ours */
static struct { int magic; } nal_device = { 0x4e414c44 }, nal_context = { 0x4e414c43 };
#define FAKE_DEVICE ((ALCdevice *)&nal_device)
#define FAKE_CONTEXT ((ALCcontext *)&nal_context)

static void nal_say(const char *fmt, ...)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "AL[%d %s]: ", getpid(), getprogname());
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", line);
	FILE *f = fopen("/var/local/gl.log", "a");
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
}

static double nal_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- the model ---- */
#define NAL_MAX 1024          /* ids 1..NAL_MAX-1 for each of buffers and sources */
#define NAL_QUEUE 128

struct nal_buffer {
	int used;
	double seconds;       /* length of its data */
};

struct nal_source {
	int used;
	int state;            /* AL_INITIAL, AL_PLAYING, AL_STOPPED */
	int looping;
	int type;             /* AL_UNDETERMINED, AL_STATIC, AL_STREAMING */
	ALuint queue[NAL_QUEUE];
	int queued;
	double started;       /* when the buffer at queue[0] began playing */
	float gain, pitch;
};

static struct nal_buffer nal_buffers[NAL_MAX];
static struct nal_source nal_sources[NAL_MAX];

static struct nal_source *nal_src(ALuint id)
{
	return (id > 0 && id < NAL_MAX && nal_sources[id].used) ? &nal_sources[id] : NULL;
}

static double nal_len(ALuint buf)
{
	return (buf > 0 && buf < NAL_MAX && nal_buffers[buf].used) ? nal_buffers[buf].seconds : 0;
}

/* how many queued buffers have finished; a source that ran past its end stops (unless it loops) */
static int nal_processed(struct nal_source *s)
{
	if (s->state == AL_STOPPED)
		return s->queued;
	if (s->state != AL_PLAYING)
		return 0;
	double t = nal_now() - s->started, total = 0;
	for (int i = 0; i < s->queued; i++)
		total += nal_len(s->queue[i]);
	if (s->looping && total > 0) {
		while (t >= total) {
			t -= total;
			s->started += total;
		}
		return 0;
	}
	int done = 0;
	for (int i = 0; i < s->queued; i++) {
		double l = nal_len(s->queue[i]);
		if (t < l)
			break;
		t -= l;
		done++;
	}
	if (done == s->queued)
		s->state = AL_STOPPED;
	return done;
}

/* ---- interposers ---- */
static ALCdevice *nal_alcOpenDevice(const ALCchar *name)
{
	ALCdevice *d = alcOpenDevice(name);
	if (d)
		return d;
	pthread_mutex_lock(&nal_lock);
	if (!nal_fake)
		nal_say("redwood-al: OpenAL has no device (%s): sounds go to a silent stand-in", name ? name : "default");
	nal_fake = 1;
	pthread_mutex_unlock(&nal_lock);
	return FAKE_DEVICE;
}

static ALCboolean nal_alcCloseDevice(ALCdevice *d)
{
	return d == FAKE_DEVICE ? 1 : alcCloseDevice(d);
}

static ALCcontext *nal_alcCreateContext(ALCdevice *d, const ALCint *attrs)
{
	return d == FAKE_DEVICE ? FAKE_CONTEXT : alcCreateContext(d, attrs);
}

static ALCboolean nal_alcMakeContextCurrent(ALCcontext *c)
{
	return (c == FAKE_CONTEXT || (nal_fake && !c)) ? 1 : alcMakeContextCurrent(c);
}

static void nal_alcProcessContext(ALCcontext *c)
{
	if (c != FAKE_CONTEXT)
		alcProcessContext(c);
}

static void nal_alcSuspendContext(ALCcontext *c)
{
	if (c != FAKE_CONTEXT)
		alcSuspendContext(c);
}

static void nal_alcDestroyContext(ALCcontext *c)
{
	if (c != FAKE_CONTEXT)
		alcDestroyContext(c);
}

static void nal_alGenBuffers(ALsizei n, ALuint *ids)
{
	if (!nal_fake) {
		alGenBuffers(n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	for (ALsizei i = 0, id = 1; i < n; i++) {
		while (id < NAL_MAX && nal_buffers[id].used)
			id++;
		if (id >= NAL_MAX) {
			ids[i] = 0;
			continue;
		}
		memset(&nal_buffers[id], 0, sizeof(nal_buffers[id]));
		nal_buffers[id].used = 1;
		ids[i] = id;
	}
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alDeleteBuffers(ALsizei n, const ALuint *ids)
{
	if (!nal_fake) {
		alDeleteBuffers(n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	for (ALsizei i = 0; i < n; i++)
		if (ids[i] > 0 && ids[i] < NAL_MAX)
			nal_buffers[ids[i]].used = 0;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alBufferData(ALuint buf, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq)
{
	if (!nal_fake) {
		alBufferData(buf, format, data, size, freq);
		return;
	}
	int frame = format == AL_FORMAT_MONO8 ? 1 : format == AL_FORMAT_STEREO16 ? 4 : 2;
	pthread_mutex_lock(&nal_lock);
	if (buf > 0 && buf < NAL_MAX && nal_buffers[buf].used)
		nal_buffers[buf].seconds = (freq > 0 && size > 0) ? (double)size / frame / freq : 0;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alGenSources(ALsizei n, ALuint *ids)
{
	if (!nal_fake) {
		alGenSources(n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	for (ALsizei i = 0, id = 1; i < n; i++) {
		while (id < NAL_MAX && nal_sources[id].used)
			id++;
		if (id >= NAL_MAX) {
			ids[i] = 0;
			continue;
		}
		memset(&nal_sources[id], 0, sizeof(nal_sources[id]));
		nal_sources[id].used = 1;
		nal_sources[id].state = AL_INITIAL;
		nal_sources[id].type = AL_UNDETERMINED;
		nal_sources[id].gain = nal_sources[id].pitch = 1;
		ids[i] = id;
	}
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alDeleteSources(ALsizei n, const ALuint *ids)
{
	if (!nal_fake) {
		alDeleteSources(n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	for (ALsizei i = 0; i < n; i++)
		if (ids[i] > 0 && ids[i] < NAL_MAX)
			nal_sources[ids[i]].used = 0;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSourcef(ALuint src, ALenum p, ALfloat v)
{
	if (!nal_fake) {
		alSourcef(src, p, v);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s && p == AL_GAIN)
		s->gain = v;
	if (s && p == AL_PITCH)
		s->pitch = v;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSource3f(ALuint src, ALenum p, ALfloat a, ALfloat b, ALfloat c)
{
	if (!nal_fake)
		alSource3f(src, p, a, b, c);
}

static void nal_alSourcei(ALuint src, ALenum p, ALint v)
{
	if (!nal_fake) {
		alSourcei(src, p, v);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s && p == AL_LOOPING)
		s->looping = v != 0;
	if (s && p == AL_BUFFER) {
		s->queued = 0;
		s->type = AL_UNDETERMINED;
		if (v) {
			s->queue[0] = (ALuint)v;
			s->queued = 1;
			s->type = AL_STATIC;
		}
	}
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alGetSourcef(ALuint src, ALenum p, ALfloat *v)
{
	if (!nal_fake) {
		alGetSourcef(src, p, v);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	float r = 0;
	if (s && p == AL_GAIN)
		r = s->gain;
	else if (s && p == AL_PITCH)
		r = s->pitch;
	else if (s && p == AL_SEC_OFFSET && s->state == AL_PLAYING) {
		nal_processed(s);
		if (s->state == AL_PLAYING)
			r = nal_now() - s->started;
	}
	if (v)
		*v = r;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alGetSourcei(ALuint src, ALenum p, ALint *v)
{
	if (!nal_fake) {
		alGetSourcei(src, p, v);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	int r = 0;
	if (s) {
		int done = nal_processed(s);
		switch (p) {
		case AL_SOURCE_STATE: r = s->state; break;
		case AL_BUFFERS_QUEUED: r = s->queued; break;
		case AL_BUFFERS_PROCESSED: r = done; break;
		case AL_LOOPING: r = s->looping; break;
		case AL_BUFFER: r = (s->type == AL_STATIC && s->queued) ? (int)s->queue[0] : 0; break;
		case AL_SOURCE_TYPE: r = s->type; break;
		}
	}
	if (v)
		*v = r;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSourcePlay(ALuint src)
{
	if (!nal_fake) {
		alSourcePlay(src);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s && s->state != AL_PLAYING) {
		s->state = AL_PLAYING;
		s->started = nal_now();
	}
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSourceStop(ALuint src)
{
	if (!nal_fake) {
		alSourceStop(src);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s)
		s->state = AL_STOPPED;
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSourceQueueBuffers(ALuint src, ALsizei n, const ALuint *ids)
{
	if (!nal_fake) {
		alSourceQueueBuffers(src, n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s) {
		if (s->type == AL_STATIC)
			s->queued = 0;
		s->type = AL_STREAMING;
		/* a stopped stream that gets new buffers starts over on its next play */
		for (ALsizei i = 0; i < n && s->queued < NAL_QUEUE; i++)
			s->queue[s->queued++] = ids[i];
	}
	pthread_mutex_unlock(&nal_lock);
}

static void nal_alSourceUnqueueBuffers(ALuint src, ALsizei n, ALuint *ids)
{
	if (!nal_fake) {
		alSourceUnqueueBuffers(src, n, ids);
		return;
	}
	pthread_mutex_lock(&nal_lock);
	struct nal_source *s = nal_src(src);
	if (s) {
		int done = nal_processed(s);
		if (n > done)
			n = done;       /* OpenAL refuses to unqueue unprocessed buffers */
		for (ALsizei i = 0; i < n; i++) {
			ids[i] = s->queue[i];
			if (s->state == AL_PLAYING)
				s->started += nal_len(s->queue[i]);
		}
		memmove(s->queue, s->queue + n, (s->queued - n) * sizeof(ALuint));
		s->queued -= n;
	}
	pthread_mutex_unlock(&nal_lock);
}

#define NAL_INTERPOSE(name)                                                                             \
	__attribute__((used)) static const struct {                                                     \
		const void *replacement_;                                                               \
		const void *replacee_;                                                                  \
	} nal_interpose_##name __attribute__((section("__DATA,__interpose"))) = {                     \
		(const void *)(uintptr_t)&nal_##name, (const void *)(uintptr_t)&name                    \
	};

NAL_INTERPOSE(alcOpenDevice)
NAL_INTERPOSE(alcCloseDevice)
NAL_INTERPOSE(alcCreateContext)
NAL_INTERPOSE(alcMakeContextCurrent)
NAL_INTERPOSE(alcProcessContext)
NAL_INTERPOSE(alcSuspendContext)
NAL_INTERPOSE(alcDestroyContext)
NAL_INTERPOSE(alGenBuffers)
NAL_INTERPOSE(alDeleteBuffers)
NAL_INTERPOSE(alBufferData)
NAL_INTERPOSE(alGenSources)
NAL_INTERPOSE(alDeleteSources)
NAL_INTERPOSE(alSourcef)
NAL_INTERPOSE(alSource3f)
NAL_INTERPOSE(alSourcei)
NAL_INTERPOSE(alGetSourcef)
NAL_INTERPOSE(alGetSourcei)
NAL_INTERPOSE(alSourcePlay)
NAL_INTERPOSE(alSourceStop)
NAL_INTERPOSE(alSourceQueueBuffers)
NAL_INTERPOSE(alSourceUnqueueBuffers)
