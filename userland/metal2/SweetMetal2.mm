/*
 * SweetMetal2.bundle: the Metal driver for the Adreno 640 on metal2vulkan + Turnip (boot-arg sweetmtl=2: the kernel's
 * SweetAccelerator names this bundle and class SweetMTL2Device). Metal.framework loads it in every process that asks
 * for a device; -initWithAcceleratorPort: hands back the frontend's device (NativeBridgeMetalFrontend.mm, UniXNU's,
 * over the bridge in bridge/, which drives Turnip, the libvulkan_freedreno.dylib inside this bundle, through the
 * kernel's SweetAcceleratorUserClient, xnu patch 0144). Which processes get one: those /var/local/mtl2-clients
 * lists ("*" = all, "-NAME" excludes NAME); the rest get none, so their frameworks take their CPU paths. guacamole:
 * backboardd too only when listed (ios-flame gives it one always), since it composites through freedreno GL.
 * Log: /dev/console and /var/local/mtl2.log (when it exists, mode 666), lines "MTL2[pid name]: ...".
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

extern "C" void sm2_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "MTL2[%d %s]: %s\n", getpid(), getprogname(), s);
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;
	static int console = -2, file = -2;
	if (console == -2)
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (file == -2)
		file = open("/var/local/mtl2.log", O_WRONLY | O_APPEND);
	if (console >= 0)
		write(console, buf, (size_t)n);
	if (file >= 0)
		write(file, buf, (size_t)n);
}

static void sm2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm2_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sm2_logs(buf);
}

/*
 * The A640 does not snoop the CPU caches: libSystem's sys_dcache_flush (a no-op where the GPU is coherent) must
 * really clean and invalidate here. User space may (SCTLR_EL1.UCI). This definition wins at link time, for the
 * bridge and the frontend alike (IOSurface imports).
 */
extern "C" void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Metal validation failures abort by default; frameworks with Apple-GPU-only kernels (MPS) report one on any other device. */
__attribute__((constructor)) static void sm2_error_mode(void)
{
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);	/* 5 = NSLog */
}

/* Selectors the frontend lacks, each logged once (the frontend then raises the usual unrecognised-selector exception) */
static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm2_log("MISSING %s -%s", cls, sel);
}

@interface NBDeviceMethods : NSObject
@end

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines itself. */
@interface NBDeviceMethods (SweetMetal2)
@end
@implementation NBDeviceMethods (SweetMetal2)
/*
 * CoreImage's CIMetalDeviceGetLimits asks it (private: the memory the GPU shares with the CPU) inside a dispatch_once:
 * unanswered, the exception left that once unfinished and every later CoreImage user waited on it (Telegram's photo
 * editor and its chat videos hung, mtl2 run 10). What a client may hold of the kernel's GPU pool (0147: 768 MB a
 * process) rounded to 1 GB; CoreImage sizes its caches by it.
 */
- (NSUInteger)sharedMemorySize
{
	return 1ull << 30;
}
/* The same probe asks these next: no memory of the GPU's own (unified), and the kernel's per-process pool cap */
- (NSUInteger)dedicatedMemorySize
{
	return 0;
}
- (uint64_t)recommendedMaxWorkingSetSize
{
	return 768ull << 20;
}
- (NSUInteger)currentAllocatedSize
{
	return 0;
}
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	if (error)
		*error = [NSError errorWithDomain:@"SweetMetal2" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
@end

static bool listed(void)
{
	FILE *f = fopen("/var/local/mtl2-clients", "r");
	if (!f)
		return false;
	char text[2048] = { 0 };
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	/* names, "*" = all, "-NAME" = never NAME (wins over "*") */
	char *save = NULL;
	bool yes = false;
	for (char *name = strtok_r(text, " ,\t\r\n", &save); name; name = strtok_r(NULL, " ,\t\r\n", &save)) {
		if (name[0] == '-' && !strcmp(name + 1, getprogname()))
			return false;
		if (!strcmp(name, "*") || !strcmp(name, getprogname()))
			yes = true;
	}
	return yes;
}

/*
 * The kernel starts the Adreno asynchronously (sweetgpu=11). Metal can be
 * requested by SpringBoard before RedwoodGPU has registered, and its first
 * failed IOServiceOpen is cached for the process lifetime below. Wait for the
 * backend's readiness service before making that one-shot attempt. Override
 * the 120-second default with sweetmtlwait=N; zero disables the wait.
 */
static long metal_wait_seconds(void)
{
	char args[2048] = { 0 };
	size_t size = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0)
		return 120;
	args[sizeof(args) - 1] = 0;
	const char *key = "sweetmtlwait=";
	char *p = strstr(args, key);
	if (!p)
		return 120;
	char *end = NULL;
	long value = strtol(p + strlen(key), &end, 10);
	return end == p + strlen(key) || value < 0 ? 120 : value;
}

static io_service_t wait_for_redwood_gpu(long patience, long *waited)
{
	*waited = 0;
	io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodGPU"));
	while (!gpu && *waited < patience) {
		if (*waited == 0 || *waited % 10 == 0)
			sm2_log("waiting for RedwoodGPU: %ld s of %ld", *waited, patience);
		sleep(1);
		++*waited;
		gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodGPU"));
	}
	return gpu;
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTL2Device : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTL2Device
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	/* this object is dropped for the frontend's device: initialised, so its -[_MTLDevice dealloc] has queues to release */
	if (!(self = [super init]))
		return nil;
	static id device;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		if (!listed()) {
			sm2_log("not listed (/var/local/mtl2-clients): no Metal device");
			return;
		}
		long waited = 0;
		long patience = metal_wait_seconds();
		io_service_t gpu = wait_for_redwood_gpu(patience, &waited);
		if (gpu) {
			IOObjectRelease(gpu);
			sm2_log("RedwoodGPU ready after %ld s; opening Metal", waited);
		} else {
			sm2_log("RedwoodGPU absent after %ld s; trying Metal open", waited);
		}
		NSString *bundle = [[NSBundle bundleForClass:[SweetMTL2Device class]] bundlePath];
		NSString *turnip = [bundle stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
		/*
		 * The shader cache needs a directory this process may create files in. /var/local is root 0755, so only root
		 * daemons can mkdir there; a sandboxed app is refused (EACCES) even when the directory exists. TMPDIR is the
		 * one directory every process owns, so try it first and keep /var/local for the daemons.
		 */
		char cache[256] = { 0 };
		char cand[3][256];
		int ncand = 0;
		const char *prog = getprogname();
		mkdir("/var/local/m2v-cache", 0777);
		chmod("/var/local/m2v-cache", 0777);
		const char *tmp = getenv("TMPDIR");
		if (tmp && tmp[0] == '/')
			snprintf(cand[ncand++], sizeof(cand[0]), "%sm2v-%s", tmp + (tmp[strlen(tmp) - 1] == '/'), prog);
		snprintf(cand[ncand++], sizeof(cand[0]), "/var/local/m2v-cache/%s", prog);
		char dir[256];
		if (confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir) - 8))
			snprintf(cand[ncand++], sizeof(cand[0]), "%sm2v-%s", dir, prog);
		for (int i = 0; i < ncand && !cache[0]; i++) {
			mkdir(cand[i], 0777);
			if (access(cand[i], R_OK | W_OK | X_OK) == 0)
				snprintf(cache, sizeof(cache), "%s", cand[i]);
			else
				sm2_log("cache candidate %s: %s", cand[i], strerror(errno));
		}
		if (!cache[0])
			snprintf(cache, sizeof(cache), "%s", cand[0]);
		/* plain rows everywhere: no Apple framebuffer compression, no 10-bit deep framebuffer */
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		/* the runtime's VkPipeline makings count into the telemetry (V row) */
		setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
		/* debugging: KEY=VALUE lines of /var/local/mtl2-env into this process's environment */
		if (FILE *ef = fopen("/var/local/mtl2-env", "r")) {
			char line[256];
			while (fgets(line, sizeof(line), ef)) {
				line[strcspn(line, "\r\n")] = 0;
				char *eq = strchr(line, '=');
				if (line[0] != '#' && eq) {
					*eq = 0;
					setenv(line, eq + 1, 1);
					sm2_log("env %s=%s", line, eq + 1);
				}
			}
			fclose(ef);
		}
		/* debugging: Turnip options for this process from /var/local/mtl2-tu-debug (TU_DEBUG), its log to /var/local/turnip-<proc>.log */
		{
			char opts[128] = { 0 };
			FILE *f = fopen("/var/local/mtl2-tu-debug", "r");
			if (f) {
				if (fgets(opts, sizeof(opts), f))
					opts[strcspn(opts, "\r\n")] = 0;
				fclose(f);
			}
			if (opts[0]) {
				char path[128];
				snprintf(path, sizeof(path), "/var/local/turnip-%s.log", getprogname());
				setenv("TU_DEBUG", opts, 1);
				setenv("MESA_LOG_FILE", path, 1);
				sm2_log("TU_DEBUG=%s, log %s", opts, path);
			}
		}
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		double t0 = CFAbsoluteTimeGetCurrent();
		@try {
			device = native_bridge_metal_open(turnip.UTF8String, cache, registryID, missing);
		} @catch (NSException *e) {
			sm2_log("device: exception %s", e.reason.UTF8String);
			device = nil;
		}
		sm2_log("device %s in %.0f ms (cache %s)%s%s", device ? [[device name] UTF8String] : "refused",
			(CFAbsoluteTimeGetCurrent() - t0) * 1000, cache, device ? "" : ": ", device ? "" : nmb_last_error());
		/*
		 * Extended-range (XR10) formats: the frontend withholds them so backboardd keeps an 8-bit framebuffer; other
		 * processes may render into them (AegirPoster's CAMetalLayer throws "unsupported extended range format" without).
		 */
		if (device && strcmp(getprogname(), "backboardd") != 0)
			class_replaceMethod(object_getClass(device), sel_registerName("supportsExtendedXR10Formats"),
					    imp_implementationWithBlock(^BOOL(id me) { (void)me; return YES; }), "B16@0:8");
		/* image bases, for symbolicating pstack samples (atos -o out/SweetMetal2.dSYM -l BASE ADDR) */
		Dl_info self = { 0 }, tu = { 0 };
		dladdr((void *)sm2_logs, &self);
		/* load it here first: a failure is named here instead of surfacing as a bridge "device" error */
		void *th = dlopen(turnip.UTF8String, RTLD_NOW);
		if (!th) {
			const char *why = dlerror();
			sm2_log("libvulkan_freedreno %s: %s", turnip.UTF8String, why ? why : "does not load");
		}
		void *vk = th ? dlsym(th, "vk_icdGetInstanceProcAddr") : NULL;
		if (vk)
			dladdr(vk, &tu);
		sm2_log("images: SweetMetal2 %p, libvulkan_freedreno %p", self.dli_fbase, tu.dli_fbase);
		/* backboardd: the frontend's counters every 10 s, into /var/local/mtl2.log only (with /var/local/mtl2-telemetry) */
		if (device && !strcmp(getprogname(), "backboardd"))
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				for (;;) {
					sleep(10);
					if (access("/var/local/mtl2-telemetry", F_OK) != 0)
						continue;
					static char text[8192];
					native_bridge_metal_telemetry(text, sizeof(text));
					int fd = open("/var/local/mtl2.log", O_WRONLY | O_APPEND);
					if (fd >= 0) {
						dprintf(fd, "MTL2[%d %s]: telemetry\n%s", getpid(), getprogname(), text);
						close(fd);
					}
				}
			});
	});
	return device;
}
@end
