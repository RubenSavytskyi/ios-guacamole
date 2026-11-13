/*
 * SweetCodec: a HAL AudioServerPlugIn standing in for the iPhone's audio codec driver.
 *
 * mediaserverd's VirtualAudio.plugin (the iOS routing layer) builds its routes from the HAL's built-in
 * devices: it looks them up by name ("Codec", "Speaker", "Sidetone", ...; Device_Codec_Aspen.cpp) and
 * takes the codec's ports from the device. guacamole has no such driver, so VirtualAudio knows only the
 * "Application Processor" source and Null_Device, every route ends there, the volume has nothing to
 * control and Bluetooth A2DP never becomes a destination. This plug-in publishes a built-in device named
 * "Codec" with an output stream (data sources speaker / receiver / wired headphones) and an input stream
 * (built-in mic): no hardware behind it, IO is dropped, the clock is host time. First goal: VirtualAudio
 * accepts it as the codec and routes playback to Bluetooth and the volume to a real control; the DSP /
 * speaker hardware can be attached later.
 *
 * Shape as Apple's NullAudio sample (AudioServerPlugIn: plug-in -> device -> streams, controls).
 * Built against the macOS SDK's CoreAudio headers (the iOS SDK hides them); the interface is the same
 * (BTAudioHALPlugin, AppleAOPAudioPlugin use it). MIT, as userland/.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "sweetpcm.h"

/* AudioHardware.h (not in AudioHardwareBase.h) */
enum {
	kAudioDevicePropertyDataSource = 'ssrc',
	kAudioDevicePropertyDataSources = 'ssc#',
	kAudioDevicePropertyDataSourceNameForIDCFString = 'lscn',
	kAudioDevicePropertyDataSourceKindForID = 'ssck',
};

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "SWEET CODEC: " fmt, ##__VA_ARGS__)

enum {
	kObj_PlugIn = kAudioObjectPlugInObject,
	kObj_Device = 2,
	kObj_OutStream = 3,
	kObj_InStream = 4,
	kObj_OutVolume = 5,	/* master (element 0) */
	kObj_OutMute = 6,
	kObj_OutSource = 7,
	kObj_InSource = 8,
	kObj_FirstElemCtl = 10,	/* per-element volume/mute pairs, see elem_ctls[] */
	kObj_HawkDevice = 30,	/* second device, "Hawking": see below */
	kObj_HawkInStream = 31,
	kObj_HawkVolume = 32,	/* input element 0 */
	kObj_HawkMute = 33,
};

/*
 * "Hawking" is the haptics (audio-haptic) device. VirtualAudio's default VAD requires a mandatory
 * alternate VAD of type vhaw on port types { 'phki', 'pnli' } for every playback route (RoutingManager.cpp
 * 4383, "VAD is Mandatory"); without a 'phki' port every route but AP -> Null_Device fails ("Recursion
 * failed after skipping optional alternate VAD"), speaker and Bluetooth A2DP included. Device_Hawking_Aspen
 * (found by the name "Hawking", DeviceFactory_Aspen) makes the 'phki' port ("Hawking Input") from input
 * element 0 and puts a volume and a mute control on it ({inpt, 0}).
 */
#define HAWK_NAME "Hawking"
#define HAWK_UID  "Hawking"
static Float32 hawk_volume = 1.0f;
static UInt32 hawk_mute;

static Boolean is_hawk(AudioObjectID obj) { return obj >= kObj_HawkDevice && obj <= kObj_HawkMute; }

/*
 * VirtualAudio's Device_Codec creates a port per HAL element that has a mute (and volume) control:
 * output element 4 -> speaker (pspk), 3 -> receiver (prec), 1 -> wired headphones (phpw, plus 'jack'),
 * 2 -> TTY, 5 -> hearing aid, 6/8 -> line out; input element 0 with mute -> built-in / wired mic.
 * (Device_Codec_Aspen.cpp, AudioObjectHasProperty on {mute/volm, scope, element}.)
 */
static const struct {
	AudioObjectPropertyScope scope;
	UInt32 element;
} elem_ctls[] = {
	{ kAudioObjectPropertyScopeOutput, 4 },	/* speaker */
	{ kAudioObjectPropertyScopeOutput, 3 },	/* receiver */
	{ kAudioObjectPropertyScopeInput, 0 },	/* microphone */
};
#define N_ELEM_CTLS (sizeof(elem_ctls) / sizeof(elem_ctls[0]))
#define ELEM_VOL(i)  (kObj_FirstElemCtl + 2 * (i))
#define ELEM_MUTE(i) (kObj_FirstElemCtl + 2 * (i) + 1)
#define kObj_LastElemCtl (kObj_FirstElemCtl + 2 * N_ELEM_CTLS - 1)
static Float32 elem_volume[N_ELEM_CTLS] = { 1.0f, 1.0f, 1.0f };
static UInt32 elem_mute[N_ELEM_CTLS];

/* which element control an object is: index into elem_ctls or -1; *is_mute set */
static int elem_ctl_of(AudioObjectID obj, Boolean *is_mute)
{
	if (obj < kObj_FirstElemCtl || obj > kObj_LastElemCtl)
		return -1;
	*is_mute = (obj - kObj_FirstElemCtl) & 1;
	return (int)((obj - kObj_FirstElemCtl) / 2);
}

#define DEVICE_NAME "Codec"
#define DEVICE_UID  "Codec"
#define MODEL_UID   "sweet-codec"
#define MAKER       "Pancakes.png"
#define BUNDLE_ID   "com.sweet.audio.codec"

/* output data sources, named as VirtualAudio's port types (Device_Codec_Aspen: pspk prec phpw plou) */
static const UInt32 out_sources[] = { 'pspk', 'prec', 'phpw' };
static const char *out_source_names[] = { "Speaker", "Receiver", "Headphones" };
static const UInt32 in_sources[] = { 'pmbi' };
static const char *in_source_names[] = { "Built-In Mic" };

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef host;
static Float64 sample_rate = 48000.0;
static UInt32 io_running;
static Float64 host_ticks_per_frame;
static UInt64 anchor_host_time;
static UInt64 periods;
static Float32 out_volume = 1.0f;
static UInt32 out_mute;
static UInt32 out_source = 'pspk', in_source = 'pmbi';
static atomic_uint refs = 1;

#define ZERO_TS_PERIOD 4096

static Float64 ts_period_host(void) { return host_ticks_per_frame * ZERO_TS_PERIOD; }

/* ---- COM plumbing ---- */

static AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gRef = &gInterfacePtr;

void *SweetCodecFactory(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
	(void)allocator;
	if (CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
		LOG("factory: plug-in requested");
		return gRef;
	}
	return NULL;
}

static HRESULT QueryInterface(void *self, REFIID iid, LPVOID *out)
{
	(void)self;
	CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
	HRESULT r = E_NOINTERFACE;
	if (CFEqual(u, IUnknownUUID) || CFEqual(u, kAudioServerPlugInDriverInterfaceUUID)) {
		atomic_fetch_add(&refs, 1);
		*out = gRef;
		r = S_OK;
	}
	CFRelease(u);
	return r;
}
static ULONG AddRef(void *self) { (void)self; return atomic_fetch_add(&refs, 1) + 1; }
static ULONG Release(void *self) { (void)self; return atomic_fetch_sub(&refs, 1) - 1; }

static OSStatus Initialize(AudioServerPlugInDriverRef self, AudioServerPlugInHostRef h)
{
	(void)self;
	host = h;
	struct mach_timebase_info tb;
	mach_timebase_info(&tb);
	host_ticks_per_frame = ((Float64)tb.denom / tb.numer) * 1e9 / sample_rate;
	LOG("initialized: device \"%s\" at %.0f Hz", DEVICE_NAME, sample_rate);
	return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef s, CFDictionaryRef d, const AudioServerPlugInClientInfo *c, AudioObjectID *o)
{ (void)s; (void)d; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef s, AudioObjectID o) { (void)s; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef s, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{ (void)s; (void)o; LOG("client %u (pid %d) attached", c->mClientID, c->mProcessID); return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef s, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{ (void)s; (void)o; (void)c; return 0; }
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef s, AudioObjectID o, UInt64 action, void *info)
{
	(void)s; (void)o; (void)info;
	pthread_mutex_lock(&state_mutex);
	if (action == 48000) {
		sample_rate = (Float64)action;
		struct mach_timebase_info tb;
		mach_timebase_info(&tb);
		host_ticks_per_frame = ((Float64)tb.denom / tb.numer) * 1e9 / sample_rate;
		LOG("sample rate now %.0f Hz", sample_rate);
	}
	pthread_mutex_unlock(&state_mutex);
	return 0;
}
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef s, AudioObjectID o, UInt64 a, void *i) { (void)s; (void)o; (void)a; (void)i; return 0; }

/* ---- properties ---- */

static CFStringRef cfs(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static Boolean has_prop(AudioObjectID obj, const AudioObjectPropertyAddress *a)
{
	switch (obj) {
	case kObj_PlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyResourceBundle:
			return true;
		}
		return false;
	case kObj_Device: case kObj_HawkDevice:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency: case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList:
		case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
			return true;
		case kAudioDevicePropertyDataSource: case kAudioDevicePropertyDataSources:
		case kAudioDevicePropertyDataSourceNameForIDCFString: case kAudioDevicePropertyDataSourceKindForID:
			return obj == kObj_Device &&
			    (a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput);
		}
		return false;
	case kObj_OutStream: case kObj_InStream: case kObj_HawkInStream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
		case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
			return true;
		}
		return false;
	case kObj_OutVolume: case kObj_HawkVolume:
	elem_volume_props:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioControlPropertyScope: case kAudioControlPropertyElement:
		case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
		case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			return true;
		}
		return false;
	case kObj_OutMute: case kObj_HawkMute:
	elem_mute_props:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioControlPropertyScope: case kAudioControlPropertyElement: case kAudioBooleanControlPropertyValue:
			return true;
		}
		return false;
	default: {
		Boolean m;
		if (elem_ctl_of(obj, &m) >= 0) {
			if (m)
				goto elem_mute_props;
			goto elem_volume_props;
		}
		return false;
	}
	case kObj_OutSource: case kObj_InSource:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioControlPropertyScope: case kAudioControlPropertyElement:
		case kAudioSelectorControlPropertyCurrentItem: case kAudioSelectorControlPropertyAvailableItems:
		case kAudioSelectorControlPropertyItemName: case kAudioSelectorControlPropertyItemKind:
			return true;
		}
		return false;
	}
	return false;
}

static int trace = -1;	/* /var/local/codec.trace: log every query (which selectors VirtualAudio asks for) */

static void trace_q(const char *what, AudioObjectID obj, const AudioObjectPropertyAddress *a, Boolean has)
{
	if (trace < 0)
		trace = access("/var/local/codec.trace", F_OK) == 0;
	if (!trace || !has)
		return;
	LOG("%s obj %u '%c%c%c%c' scope '%c%c%c%c' elem %u", what, obj, a->mSelector >> 24, a->mSelector >> 16,
	    a->mSelector >> 8, a->mSelector, a->mScope >> 24, a->mScope >> 16, a->mScope >> 8, a->mScope, a->mElement);
}

static Boolean HasProperty(AudioServerPlugInDriverRef s, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
	(void)s; (void)pid;
	Boolean h = has_prop(obj, a);
	if (trace < 0)
		trace = access("/var/local/codec.trace", F_OK) == 0;
	if (trace && !h)
		LOG("has? obj %u '%c%c%c%c' scope '%c%c%c%c' elem %u: no", obj, a->mSelector >> 24, a->mSelector >> 16,
		    a->mSelector >> 8, a->mSelector, a->mScope >> 24, a->mScope >> 16, a->mScope >> 8, a->mScope, a->mElement);
	return h;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef s, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
	(void)s; (void)pid;
	if (!has_prop(obj, a))
		return kAudioHardwareUnknownPropertyError;
	*out = false;
	if ((obj == kObj_Device || obj == kObj_HawkDevice) &&
	    (a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kAudioDevicePropertyDataSource))
		*out = true;
	if ((obj == kObj_OutStream || obj == kObj_InStream || obj == kObj_HawkInStream) &&
	    (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat || a->mSelector == kAudioStreamPropertyIsActive))
		*out = true;
	Boolean em;
	int ei = elem_ctl_of(obj, &em);
	if ((obj == kObj_OutVolume || obj == kObj_HawkVolume || (ei >= 0 && !em)) &&
	    (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue))
		*out = true;
	if ((obj == kObj_OutMute || obj == kObj_HawkMute || (ei >= 0 && em)) && a->mSelector == kAudioBooleanControlPropertyValue)
		*out = true;
	if ((obj == kObj_OutSource || obj == kObj_InSource) && a->mSelector == kAudioSelectorControlPropertyCurrentItem)
		*out = true;
	return 0;
}

static AudioStreamBasicDescription fmt_for(Float64 rate, UInt32 ch)
{
	AudioStreamBasicDescription f = { 0 };
	f.mSampleRate = rate;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
	f.mBytesPerPacket = 4 * ch;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4 * ch;
	f.mChannelsPerFrame = ch;
	f.mBitsPerChannel = 32;
	return f;
}

#define RET_U32(v)   do { if (in < 4) return kAudioHardwareBadPropertySizeError; *(UInt32 *)data = (v); *out = 4; return 0; } while (0)
#define RET_F64(v)   do { if (in < 8) return kAudioHardwareBadPropertySizeError; *(Float64 *)data = (v); *out = 8; return 0; } while (0)
#define RET_F32(v)   do { if (in < 4) return kAudioHardwareBadPropertySizeError; *(Float32 *)data = (v); *out = 4; return 0; } while (0)
#define RET_CFS(str) do { if (in < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; *(CFStringRef *)data = cfs(str); *out = sizeof(CFStringRef); return 0; } while (0)
#define RET_IDS(arr, n) do { UInt32 k = (UInt32)((in / 4) < (n) ? (in / 4) : (n)); memcpy(data, (arr), k * 4); *out = k * 4; return 0; } while (0)

static OSStatus size_of(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 *out)
{
	(void)qsize; (void)qdata;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
	case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel: case kAudioControlPropertyScope:
	case kAudioControlPropertyElement: case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
	case kAudioBooleanControlPropertyValue: case kAudioSelectorControlPropertyCurrentItem: case kAudioDevicePropertyDataSource:
	case kAudioSelectorControlPropertyItemKind: case kAudioDevicePropertyDataSourceKindForID:
		*out = 4; return 0;
	case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID: case kAudioPlugInPropertyResourceBundle: case kAudioSelectorControlPropertyItemName:
		*out = sizeof(CFStringRef); return 0;
	case kAudioDevicePropertyDataSourceNameForIDCFString: case kAudioPlugInPropertyTranslateUIDToDevice:
		*out = obj == kObj_PlugIn ? sizeof(AudioObjectID) : sizeof(AudioValueTranslation); return 0;
	case kAudioObjectPropertyOwnedObjects:
		if (obj == kObj_PlugIn) { *out = 2 * 4; return 0; }
		if (obj == kObj_HawkDevice) { *out = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 3 * 4; return 0; }
		*out = (a->mScope == kAudioObjectPropertyScopeGlobal ? 6 + 2 * N_ELEM_CTLS :
			a->mScope == kAudioObjectPropertyScopeOutput ? 4 + 2 * 2 : 2 + 2 * 1) * 4;
		return 0;
	case kAudioPlugInPropertyDeviceList: *out = 2 * 4; return 0;
	case kAudioDevicePropertyRelatedDevices: *out = 4; return 0;
	case kAudioDevicePropertyStreams:
		if (obj == kObj_HawkDevice) { *out = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 4; return 0; }
		*out = a->mScope == kAudioObjectPropertyScopeGlobal ? 8 : 4; return 0;
	case kAudioObjectPropertyControlList:
		*out = (obj == kObj_HawkDevice ? 2 : 4 + 2 * N_ELEM_CTLS) * 4; return 0;
	case kAudioDevicePropertyNominalSampleRate: *out = 8; return 0;
	case kAudioDevicePropertyAvailableNominalSampleRates: *out = sizeof(AudioValueRange); return 0;
	case kAudioDevicePropertyPreferredChannelsForStereo: *out = 8; return 0;
	case kAudioDevicePropertyPreferredChannelLayout: *out = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription)); return 0;
	case kAudioDevicePropertyDataSources: case kAudioSelectorControlPropertyAvailableItems:
		*out = (obj == kObj_InSource || a->mScope == kAudioObjectPropertyScopeInput) ? (UInt32)sizeof(in_sources) : (UInt32)sizeof(out_sources); return 0;
	case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
		*out = sizeof(AudioStreamBasicDescription); return 0;
	case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
		*out = sizeof(AudioStreamRangedDescription); return 0;
	case kAudioLevelControlPropertyDecibelRange: *out = sizeof(AudioValueRange); return 0;
	case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
		*out = 4; return 0;
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef s, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 *out)
{
	(void)s; (void)pid;
	if (!has_prop(obj, a))
		return kAudioHardwareUnknownPropertyError;
	OSStatus r = size_of(obj, a, qs, qd, out);
	if (trace < 0)
		trace = access("/var/local/codec.trace", F_OK) == 0;
	if (trace)
		LOG("size obj %u '%c%c%c%c' scope '%c%c%c%c' elem %u -> %d, %u bytes", obj, a->mSelector >> 24, a->mSelector >> 16,
		    a->mSelector >> 8, a->mSelector, a->mScope >> 24, a->mScope >> 16, a->mScope >> 8, a->mScope, a->mElement, (int)r, *out);
	return r;
}

static const char *source_name(UInt32 id)
{
	for (unsigned i = 0; i < sizeof(out_sources) / 4; i++) if (out_sources[i] == id) return out_source_names[i];
	for (unsigned i = 0; i < sizeof(in_sources) / 4; i++) if (in_sources[i] == id) return in_source_names[i];
	return "?";
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef s, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 in, UInt32 *out, void *data)
{
	(void)s; (void)pid; (void)qs;
	if (!has_prop(obj, a))
		return kAudioHardwareUnknownPropertyError;
	trace_q("get", obj, a, true);
	if (trace)
		LOG("get obj %u '%c%c%c%c' scope '%c%c%c%c' elem %u, %u bytes offered", obj, a->mSelector >> 24,
		    a->mSelector >> 16, a->mSelector >> 8, a->mSelector, a->mScope >> 24, a->mScope >> 16, a->mScope >> 8, a->mScope, a->mElement, in);
	Boolean is_in = a->mScope == kAudioObjectPropertyScopeInput || obj == kObj_InStream || obj == kObj_InSource;
	Boolean em = false;
	int ei = elem_ctl_of(obj, &em);
	Float32 vol = ei >= 0 ? elem_volume[ei] : obj == kObj_HawkVolume ? hawk_volume : out_volume;
	if (is_hawk(obj)) {
		/* what differs from the codec; the rest (rates, formats, clock, levels) is shared below */
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
			RET_U32(obj == kObj_HawkVolume ? kAudioLevelControlClassID : obj == kObj_HawkMute ? kAudioBooleanControlClassID :
				kAudioObjectClassID);
		case kAudioObjectPropertyClass:
			RET_U32(obj == kObj_HawkDevice ? kAudioDeviceClassID : obj == kObj_HawkInStream ? kAudioStreamClassID :
				obj == kObj_HawkVolume ? kAudioVolumeControlClassID : kAudioMuteControlClassID);
		case kAudioObjectPropertyOwner: RET_U32(obj == kObj_HawkDevice ? kObj_PlugIn : kObj_HawkDevice);
		case kAudioObjectPropertyName: RET_CFS(obj == kObj_HawkDevice ? HAWK_NAME : "Hawking Input");
		case kAudioObjectPropertyOwnedObjects: {
			static const AudioObjectID own[] = { kObj_HawkInStream, kObj_HawkVolume, kObj_HawkMute };
			if (a->mScope == kAudioObjectPropertyScopeOutput) { *out = 0; return 0; }
			RET_IDS(own, 3);
		}
		case kAudioDevicePropertyDeviceUID: RET_CFS(HAWK_UID);
		case kAudioDevicePropertyModelUID: RET_CFS("sweet-hawking");
		case kAudioDevicePropertyRelatedDevices: { static const AudioObjectID dev[] = { kObj_HawkDevice }; RET_IDS(dev, 1); }
		case kAudioDevicePropertyStreams: {
			static const AudioObjectID st[] = { kObj_HawkInStream };
			if (a->mScope == kAudioObjectPropertyScopeOutput) { *out = 0; return 0; }
			RET_IDS(st, 1);
		}
		case kAudioObjectPropertyControlList: { static const AudioObjectID c[] = { kObj_HawkVolume, kObj_HawkMute }; RET_IDS(c, 2); }
		case kAudioStreamPropertyDirection: RET_U32(1);
		case kAudioStreamPropertyTerminalType: RET_U32(kAudioStreamTerminalTypeMicrophone);
		case kAudioControlPropertyScope: RET_U32(kAudioObjectPropertyScopeInput);
		case kAudioControlPropertyElement: RET_U32(0);
		case kAudioBooleanControlPropertyValue: RET_U32(hawk_mute);
		}
	}
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		if (ei >= 0) RET_U32(em ? kAudioBooleanControlClassID : kAudioLevelControlClassID);
		RET_U32(obj == kObj_PlugIn ? kAudioObjectClassID : obj == kObj_Device ? kAudioObjectClassID :
			(obj == kObj_OutStream || obj == kObj_InStream) ? kAudioObjectClassID :
			obj == kObj_OutVolume ? kAudioLevelControlClassID : obj == kObj_OutMute ? kAudioBooleanControlClassID : kAudioSelectorControlClassID);
	case kAudioObjectPropertyClass:
		if (ei >= 0) RET_U32(em ? kAudioMuteControlClassID : kAudioVolumeControlClassID);
		RET_U32(obj == kObj_PlugIn ? kAudioPlugInClassID : obj == kObj_Device ? kAudioDeviceClassID :
			(obj == kObj_OutStream || obj == kObj_InStream) ? kAudioStreamClassID :
			obj == kObj_OutVolume ? kAudioVolumeControlClassID : obj == kObj_OutMute ? kAudioMuteControlClassID : kAudioDataSourceControlClassID);
	case kAudioObjectPropertyOwner:
		RET_U32(obj == kObj_PlugIn ? kAudioObjectUnknown : obj == kObj_Device ? kObj_PlugIn : kObj_Device);
	case kAudioObjectPropertyName:
		if (obj == kObj_Device) RET_CFS(DEVICE_NAME);
		RET_CFS(obj == kObj_OutStream ? "Codec Output" : "Codec Input");
	case kAudioObjectPropertyManufacturer: RET_CFS(MAKER);
	case kAudioObjectPropertyOwnedObjects: {
		static const AudioObjectID dev[] = { kObj_Device, kObj_HawkDevice };
		static const AudioObjectID all[] = { kObj_OutStream, kObj_InStream, kObj_OutVolume, kObj_OutMute, kObj_OutSource, kObj_InSource,
						      ELEM_VOL(0), ELEM_MUTE(0), ELEM_VOL(1), ELEM_MUTE(1), ELEM_VOL(2), ELEM_MUTE(2) };
		static const AudioObjectID outs[] = { kObj_OutStream, kObj_OutVolume, kObj_OutMute, kObj_OutSource,
						       ELEM_VOL(0), ELEM_MUTE(0), ELEM_VOL(1), ELEM_MUTE(1) };
		static const AudioObjectID ins[] = { kObj_InStream, kObj_InSource, ELEM_VOL(2), ELEM_MUTE(2) };
		if (obj == kObj_PlugIn) RET_IDS(dev, 2);
		if (a->mScope == kAudioObjectPropertyScopeOutput) RET_IDS(outs, 8);
		if (a->mScope == kAudioObjectPropertyScopeInput) RET_IDS(ins, 4);
		RET_IDS(all, 12);
	}
	case kAudioPlugInPropertyDeviceList: { static const AudioObjectID dev[] = { kObj_Device, kObj_HawkDevice }; RET_IDS(dev, 2); }
	case kAudioPlugInPropertyTranslateUIDToDevice: {
		if (in < 4 || qs < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
		CFStringRef want = *(const CFStringRef *)qd;
		CFStringRef mine = cfs(DEVICE_UID), hawk = cfs(HAWK_UID);
		*(AudioObjectID *)data = !want ? kAudioObjectUnknown : CFEqual(want, mine) ? kObj_Device :
			CFEqual(want, hawk) ? kObj_HawkDevice : kAudioObjectUnknown;
		CFRelease(mine);
		CFRelease(hawk);
		*out = 4;
		return 0;
	}
	case kAudioPlugInPropertyResourceBundle: RET_CFS("");
	case kAudioDevicePropertyDeviceUID: RET_CFS(DEVICE_UID);
	case kAudioDevicePropertyModelUID: RET_CFS(MODEL_UID);
	case kAudioDevicePropertyTransportType: RET_U32(kAudioDeviceTransportTypeBuiltIn);
	case kAudioDevicePropertyRelatedDevices: { static const AudioObjectID dev[] = { kObj_Device }; RET_IDS(dev, 1); }
	case kAudioDevicePropertyClockDomain: RET_U32(0);
	case kAudioDevicePropertyDeviceIsAlive: RET_U32(1);
	case kAudioDevicePropertyDeviceIsRunning: RET_U32(io_running != 0);
	case kAudioDevicePropertyDeviceCanBeDefaultDevice: RET_U32(1);
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RET_U32(1);
	case kAudioDevicePropertyLatency: RET_U32(0);
	case kAudioDevicePropertyStreams: {
		static const AudioObjectID both[] = { kObj_OutStream, kObj_InStream };
		static const AudioObjectID o[] = { kObj_OutStream }, i[] = { kObj_InStream };
		if (a->mScope == kAudioObjectPropertyScopeOutput) RET_IDS(o, 1);
		if (a->mScope == kAudioObjectPropertyScopeInput) RET_IDS(i, 1);
		RET_IDS(both, 2);
	}
	case kAudioObjectPropertyControlList: {
		static const AudioObjectID c[] = { kObj_OutVolume, kObj_OutMute, kObj_OutSource, kObj_InSource,
						    ELEM_VOL(0), ELEM_MUTE(0), ELEM_VOL(1), ELEM_MUTE(1), ELEM_VOL(2), ELEM_MUTE(2) };
		RET_IDS(c, 10);
	}
	case kAudioDevicePropertySafetyOffset: RET_U32(64);
	case kAudioDevicePropertyNominalSampleRate: RET_F64(sample_rate);
	case kAudioDevicePropertyAvailableNominalSampleRates: {
		/* 48 kHz only: the speaker's ASM stream (adspd) runs at that rate */
		if (in < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
		AudioValueRange *r = data;
		r[0].mMinimum = r[0].mMaximum = 48000.0;
		*out = sizeof(AudioValueRange);
		return 0;
	}
	case kAudioDevicePropertyIsHidden: RET_U32(0);
	case kAudioDevicePropertyZeroTimeStampPeriod: RET_U32(ZERO_TS_PERIOD);
	case kAudioDevicePropertyPreferredChannelsForStereo: {
		if (in < 8) return kAudioHardwareBadPropertySizeError;
		((UInt32 *)data)[0] = 1; ((UInt32 *)data)[1] = 2; *out = 8; return 0;
	}
	case kAudioDevicePropertyPreferredChannelLayout: {
		UInt32 need = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription));
		if (in < need) return kAudioHardwareBadPropertySizeError;
		AudioChannelLayout *l = data;
		l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
		l->mChannelBitmap = 0;
		l->mNumberChannelDescriptions = 2;
		for (int c = 0; c < 2; c++) {
			l->mChannelDescriptions[c].mChannelLabel = c == 0 ? kAudioChannelLabel_Left : kAudioChannelLabel_Right;
			l->mChannelDescriptions[c].mChannelFlags = 0;
			l->mChannelDescriptions[c].mCoordinates[0] = l->mChannelDescriptions[c].mCoordinates[1] = l->mChannelDescriptions[c].mCoordinates[2] = 0;
		}
		*out = need;
		return 0;
	}
	case kAudioDevicePropertyDataSource: RET_U32(is_in ? in_source : out_source);
	case kAudioDevicePropertyDataSources: case kAudioSelectorControlPropertyAvailableItems:
		if (is_in) RET_IDS(in_sources, sizeof(in_sources) / 4);
		RET_IDS(out_sources, sizeof(out_sources) / 4);
	case kAudioDevicePropertyDataSourceNameForIDCFString: {
		if (in < sizeof(AudioValueTranslation)) return kAudioHardwareBadPropertySizeError;
		AudioValueTranslation *t = data;
		if (t->mInputDataSize < 4 || t->mOutputDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
		*(CFStringRef *)t->mOutputData = cfs(source_name(*(const UInt32 *)t->mInputData));
		*out = sizeof(AudioValueTranslation);
		return 0;
	}
	case kAudioDevicePropertyDataSourceKindForID: RET_U32(0);
	case kAudioSelectorControlPropertyItemName: {
		if (qs < 4) return kAudioHardwareBadPropertySizeError;
		RET_CFS(source_name(*(const UInt32 *)qd));
	}
	case kAudioSelectorControlPropertyItemKind: RET_U32(0);
	case kAudioSelectorControlPropertyCurrentItem: RET_U32(obj == kObj_InSource ? in_source : out_source);
	case kAudioStreamPropertyIsActive: RET_U32(1);
	case kAudioStreamPropertyDirection: RET_U32(obj == kObj_InStream ? 1 : 0);
	case kAudioStreamPropertyTerminalType: RET_U32(obj == kObj_InStream ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
	case kAudioStreamPropertyStartingChannel: RET_U32(1);
	case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: {
		if (in < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
		*(AudioStreamBasicDescription *)data = fmt_for(sample_rate, 2);
		*out = sizeof(AudioStreamBasicDescription);
		return 0;
	}
	case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
		if (in < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
		AudioStreamRangedDescription *r = data;
		r[0].mFormat = fmt_for(48000.0, 2); r[0].mSampleRateRange.mMinimum = r[0].mSampleRateRange.mMaximum = 48000.0;
		*out = sizeof(AudioStreamRangedDescription);
		return 0;
	}
	case kAudioControlPropertyScope:
		if (ei >= 0) RET_U32(elem_ctls[ei].scope);
		RET_U32(obj == kObj_InSource ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
	case kAudioControlPropertyElement: RET_U32(ei >= 0 ? elem_ctls[ei].element : kAudioObjectPropertyElementMain);
	case kAudioLevelControlPropertyScalarValue: RET_F32(vol);
	case kAudioLevelControlPropertyDecibelValue: RET_F32(vol <= 0.0f ? -96.0f : 20.0f * log10f(vol));
	case kAudioLevelControlPropertyDecibelRange: {
		if (in < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
		((AudioValueRange *)data)->mMinimum = -96.0; ((AudioValueRange *)data)->mMaximum = 0.0;
		*out = sizeof(AudioValueRange);
		return 0;
	}
	case kAudioLevelControlPropertyConvertScalarToDecibels: {
		if (in < 4) return kAudioHardwareBadPropertySizeError;
		Float32 v = *(Float32 *)data; *(Float32 *)data = v <= 0.0f ? -96.0f : 20.0f * log10f(v); *out = 4; return 0;
	}
	case kAudioLevelControlPropertyConvertDecibelsToScalar: {
		if (in < 4) return kAudioHardwareBadPropertySizeError;
		Float32 v = *(Float32 *)data; *(Float32 *)data = v <= -96.0f ? 0.0f : powf(10.0f, v / 20.0f); *out = 4; return 0;
	}
	case kAudioBooleanControlPropertyValue: RET_U32(ei >= 0 ? elem_mute[ei] : out_mute);
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef s, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 in, const void *data)
{
	(void)s; (void)pid; (void)qs; (void)qd;
	if (!has_prop(obj, a))
		return kAudioHardwareUnknownPropertyError;
	AudioObjectPropertyAddress changed[2];
	UInt32 nchanged = 0;
	Boolean em = false;
	int ei = elem_ctl_of(obj, &em);
	Float32 *volp = ei >= 0 ? &elem_volume[ei] : obj == kObj_HawkVolume ? &hawk_volume : &out_volume;
	UInt32 *mutep = ei >= 0 ? &elem_mute[ei] : obj == kObj_HawkMute ? &hawk_mute : &out_mute;
	AudioObjectID dev = is_hawk(obj) ? kObj_HawkDevice : kObj_Device;
	pthread_mutex_lock(&state_mutex);
	switch (a->mSelector) {
	case kAudioDevicePropertyNominalSampleRate: {
		Float64 r = in >= 8 ? *(const Float64 *)data : 0;
		pthread_mutex_unlock(&state_mutex);
		if (r != 48000.0) return kAudioHardwareIllegalOperationError;
		if (r != sample_rate && host)
			host->RequestDeviceConfigurationChange(host, dev, (UInt64)r, NULL);
		return 0;
	}
	case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: {
		const AudioStreamBasicDescription *f = data;
		Float64 r = in >= sizeof(*f) ? f->mSampleRate : 0;
		pthread_mutex_unlock(&state_mutex);
		if (r != 48000.0) return kAudioDeviceUnsupportedFormatError;
		if (r != sample_rate && host)
			host->RequestDeviceConfigurationChange(host, dev, (UInt64)r, NULL);
		return 0;
	}
	case kAudioStreamPropertyIsActive: break;
	case kAudioLevelControlPropertyScalarValue:
		if (in >= 4) { Float32 v = *(const Float32 *)data; *volp = v < 0 ? 0 : v > 1 ? 1 : v; }
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 };
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 };
		LOG("obj %u volume %.2f", obj, *volp);
		break;
	case kAudioLevelControlPropertyDecibelValue:
		if (in >= 4) { Float32 v = *(const Float32 *)data; *volp = v <= -96.0f ? 0.0f : powf(10.0f, v / 20.0f); if (*volp > 1) *volp = 1; }
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 };
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 };
		LOG("obj %u volume %.2f (dB)", obj, *volp);
		break;
	case kAudioBooleanControlPropertyValue:
		if (in >= 4) *mutep = *(const UInt32 *)data != 0;
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, 0 };
		LOG("obj %u mute %u", obj, *mutep);
		break;
	case kAudioSelectorControlPropertyCurrentItem: case kAudioDevicePropertyDataSource: {
		UInt32 v = in >= 4 ? *(const UInt32 *)data : 0;
		Boolean is_in = obj == kObj_InSource || a->mScope == kAudioObjectPropertyScopeInput;
		if (is_in) in_source = v; else out_source = v;
		changed[nchanged++] = (AudioObjectPropertyAddress){ kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, 0 };
		LOG("%s source '%c%c%c%c'", is_in ? "input" : "output", v >> 24, v >> 16, v >> 8, v);
		break;
	}
	default:
		pthread_mutex_unlock(&state_mutex);
		return kAudioHardwareUnsupportedOperationError;
	}
	pthread_mutex_unlock(&state_mutex);
	if (nchanged && host)
		host->PropertiesChanged(host, obj, nchanged, changed);
	return 0;
}

/*
 * ---- IO: the clock is host time; the codec's output mix goes to the speakers through adspd ----
 * WriteMix of the output stream is converted to 16-bit stereo with the volume / mute VirtualAudio set and
 * appended to adspd's ring (sweetpcm.h), which adspd feeds to the ADSP's ASM stream (`adspd ... pcm on`).
 * Without the ring (adspd not running) the mix is dropped, as before.
 */
static struct sweetpcm *ring;

static void ring_map(void)
{
	if (ring)
		return;
	int fd = open(SWEETPCM_PATH, O_RDWR);
	if (fd < 0) {
		LOG("no %s: speaker output dropped", SWEETPCM_PATH);
		return;
	}
	void *m = mmap(NULL, SWEETPCM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		LOG("mmap %s failed", SWEETPCM_PATH);
		return;
	}
	if (((struct sweetpcm *)m)->magic != SWEETPCM_MAGIC || ((struct sweetpcm *)m)->frames != SWEETPCM_FRAMES) {
		LOG("%s: not a ring", SWEETPCM_PATH);
		munmap(m, SWEETPCM_SIZE);
		return;
	}
	ring = m;
	LOG("speaker output through %s", SWEETPCM_PATH);
}

static void ring_write(const Float32 *src, UInt32 frames)
{
	Float32 gain = (out_mute || elem_mute[0]) ? 0.0f : out_volume * elem_volume[0];	/* master x speaker */
	uint64_t w = ring->wpos;
	for (UInt32 i = 0; i < frames; i++) {
		uint32_t k = (uint32_t)((w + i) % SWEETPCM_FRAMES);
		for (int c = 0; c < 2; c++) {
			Float32 v = src[2 * i + c] * gain * 32767.0f;
			ring->data[2 * k + c] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
		}
	}
	__atomic_store_n(&ring->wpos, w + frames, __ATOMIC_RELEASE);
}

static OSStatus StartIO(AudioServerPlugInDriverRef s, AudioObjectID dev, UInt32 client)
{
	(void)s; (void)client;
	if (dev == kObj_Device)
		ring_map();
	pthread_mutex_lock(&state_mutex);
	if (io_running++ == 0) {
		anchor_host_time = mach_absolute_time();
		periods = 0;
		LOG("IO started");
	}
	pthread_mutex_unlock(&state_mutex);
	return 0;
}
static OSStatus StopIO(AudioServerPlugInDriverRef s, AudioObjectID dev, UInt32 client)
{
	(void)s; (void)dev; (void)client;
	pthread_mutex_lock(&state_mutex);
	if (io_running && --io_running == 0)
		LOG("IO stopped");
	pthread_mutex_unlock(&state_mutex);
	return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef s, AudioObjectID dev, UInt32 client, Float64 *sample, UInt64 *hosttime, UInt64 *seed)
{
	(void)s; (void)dev; (void)client;
	pthread_mutex_lock(&state_mutex);
	UInt64 now = mach_absolute_time();
	Float64 per = ts_period_host();
	UInt64 n = (UInt64)((Float64)(now - anchor_host_time) / per);
	periods = n;
	*sample = (Float64)n * ZERO_TS_PERIOD;
	*hosttime = anchor_host_time + (UInt64)((Float64)n * per);
	*seed = 1;
	pthread_mutex_unlock(&state_mutex);
	return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef s, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will, Boolean *inplace)
{
	(void)s; (void)dev; (void)client;
	*will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
	*inplace = true;
	return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef s, AudioObjectID d, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)s; (void)d; (void)c; (void)op; (void)n; (void)i; return 0; }
static OSStatus DoIOOperation(AudioServerPlugInDriverRef s, AudioObjectID d, AudioObjectID stream, UInt32 c, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *i, void *main, void *secondary)
{
	(void)s; (void)d; (void)c; (void)i; (void)secondary;
	if (op == kAudioServerPlugInIOOperationReadInput)
		memset(main, 0, frames * 8);	/* silence from the mic */
	else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObj_OutStream && ring)
		ring_write(main, frames);
	return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef s, AudioObjectID d, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)s; (void)d; (void)c; (void)op; (void)n; (void)i; return 0; }

static AudioServerPlugInDriverInterface gInterface = {
	NULL,
	QueryInterface, AddRef, Release,
	Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
	PerformConfigChange, AbortConfigChange,
	HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
	StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
