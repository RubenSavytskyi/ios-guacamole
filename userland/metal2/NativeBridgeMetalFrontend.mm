// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: heaps, argument buffers, indirect commands, drawables/presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
#include <os/lock.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"

extern "C" void sys_dcache_flush(void *start, size_t length);

static NmbDevice *gDevice;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses,gDraws,gPassPixels,gDispatches,gBlits,gBiggestPass,gFullScreenPasses;
static std::atomic<uint64_t> gDrawablePresentQueued,gDrawablePresentIssued,gDrawablePresentSkipped,gDrawableSamples;
static std::atomic<uint64_t> gDrawableActuallyPresented;
static std::atomic<uint64_t> gWorkCommits;
static std::atomic<uint64_t> gPresentPriorWorkWaits;
static std::atomic<uint64_t> gPassDetailLogs;
static std::atomic<uint64_t> gDrawableStoreOverrides;
static std::atomic<uint64_t> gLateStoreActionLogs;
static std::atomic<uint64_t> gLateStoreOverrides;
static std::mutex gDrawableSampleLock;
static uint64_t gBlackRegionSamples[5];
static dispatch_queue_t gDrawablePresentQueue;
// 0169 (kernel patch of the same number): in backboardd, a commit that draws into IOSurfaces returns only once the
// runtime has submitted it to the GPU, so CoreAnimation's next IOMobileFramebuffer swap finds the frame's drawing
// already on the ring (the display's blit is queued behind it). Boot-arg sweetmtlsyncsubmit=1 turns it on; off by default.
static std::atomic<uint64_t> gSyncSubmitWaits;
// 0188: surfaces that already held another process's picture when this process imported them (the "seen here" probe
// found content): every use of one in an encoder is logged (first 80), whatever the shader.
static std::mutex gWatchLock;
static NSMutableSet<NSNumber *> *gWatchedSurfaces;
static std::atomic<uint32_t> gWatchLogs{0};
// 0242: the per-binding / per-draw diagnostics (0182 big-texture draw trace, 0188 watched surfaces, 0219 YCbCr
// draws) cost Objective-C allocations, associated-object locks and string formatting on every texture and sampler
// bind, pipeline change and draw. They now run only while /var/local/mtl2-trace exists (read once per process).
static bool traceOn() {
 static const bool on=access("/var/local/mtl2-trace",F_OK)==0;
 return on;
}
static bool watchedSurface(IOSurfaceRef sf) {
 if(!sf)return false;
 std::lock_guard<std::mutex> guard(gWatchLock);
 return [gWatchedSurfaces containsObject:@(IOSurfaceGetID(sf))];
}
// 0173: the drawable sampled (8x8 grid) right before CoreAnimation gets it, after its drawing has completed: frames,
// and those with >= 16 of 64 samples black. The older "frame grid" sample ran before a present-only command buffer
// waited for the frame's work, so it could read the previous frame.
static std::atomic<uint64_t> gPrePresentFrames,gPrePresentBlack;
static unsigned prePresentBlackSamples(id<MTLTexture> texture) {
 const MTLPixelFormat f=texture.pixelFormat;
 if(![(id)texture isKindOfClass:NSClassFromString(@"NBTexture")] || texture.width<16 || texture.height<16 ||
    !(f==MTLPixelFormatBGRA8Unorm || f==MTLPixelFormatRGBA8Unorm || f==MTLPixelFormatBGRA8Unorm_sRGB || f==MTLPixelFormatRGBA8Unorm_sRGB))return 0;
 unsigned black=0;
 for(unsigned j=0;j<8;j++)for(unsigned i=0;i<8;i++){
  uint8_t px[4]={};
  [texture getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(texture.width*(2*i+1)/16,texture.height*(2*j+1)/16,1,1) mipmapLevel:0];
  if(px[0]+px[1]+px[2]<=24)black++;
 }
 return black;
}
static bool syncSubmitEnabled(void) {
 static bool on;static dispatch_once_t once;
 dispatch_once(&once,^{
  if(strcmp(getprogname(),"backboardd")!=0)return;
  char args[1024]={};size_t size=sizeof(args)-1;
  if(sysctlbyname("kern.bootargs",args,&size,NULL,0)==0)on=strstr(args,"sweetmtlsyncsubmit=1")!=NULL;
 });
 return on;
}
// 0258: the per-frame black-frame sampling (64 single-pixel getBytes plus five 8x8 ones, every frame, in
// backboardd's present path) is a diagnostic; each getBytes of a Vulkan-backed drawable costs a mapping and
// may wait for the frame's own work, so it is off unless the kernel was booted with sweetmtlblack=1.
static bool blackSamplingEnabled(void) {
 static bool on;
 static dispatch_once_t once;
 dispatch_once(&once,^{
  char args[1024]={};size_t size=sizeof(args)-1;
  if(sysctlbyname("kern.bootargs",args,&size,NULL,0)==0)on=strstr(args,"sweetmtlblack=1")!=NULL;
 });
 return on;
}
static dispatch_queue_t drawablePresentQueue(void) {
 static dispatch_once_t once;
 dispatch_once(&once,^{gDrawablePresentQueue=dispatch_queue_create("org.blu-sharky.metal2.drawable-present",DISPATCH_QUEUE_SERIAL);});
 return gDrawablePresentQueue;
}
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
struct Timed {std::atomic<uint64_t> count{0},total{0},slowest{0};
 void add(uint64_t began){const uint64_t took=nowUs()-began;count++;total+=took;uint64_t seen=slowest.load();while(took>seen && !slowest.compare_exchange_weak(seen,took)){}}};
static Timed gTimePipeline,gTimeLibrary,gTimeFunction,gTimeEncodeToCommit;
static Timed gTimeWaitScheduled,gTimeWaitCompleted;
static Timed gTimeResourceWait;
static std::atomic<uint64_t> gResourceChecks,gResourceSkipped;
static Timed gTimeSurfaceFlush;
static std::atomic<uint64_t> gSurfaceFlushBytes,gSurfaceFlushDuplicates;
struct EncodedWork {uint32_t passes,draws,dispatches,blits;uint64_t pixels;};
struct CommitRecord {uint64_t committedUs=0,callbackUs=0,ageUs=0;int64_t scheduledUs=-1;uint32_t index=0;EncodedWork work={};};
static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
static std::mutex gProblemLock;
static char gLastProblems[2][200];
static void noteProblem(NSString *what) {
 gProblems++;
 std::lock_guard<std::mutex> lock(gProblemLock);
 memcpy(gLastProblems[1],gLastProblems[0],sizeof gLastProblems[0]);
 strlcpy(gLastProblems[0],what.UTF8String?:"?",sizeof gLastProblems[0]);
}
extern "C" void sm2_logs(const char *);
#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];sm2_logs(nbText.UTF8String);noteProblem(nbText);}while(0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];static unsigned gFormatCount;static std::atomic<uint64_t> gTextureRefusals;
static void noteFormat(uint32_t format,bool made) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(!made)gTextureRefusals++;
 for(unsigned i=0;i<gFormatCount;i++)if(gFormats[i]==format)return;
 if(gFormatCount<8)gFormats[gFormatCount++]=format;
}
// _MTLLibrary keeps the platform compiler's finished metallib here. On iOS 16, the public Metal API can compile
// MSL but does not expose those bytes. The bridge consumes AIR/metallib, so use this private accessor after asking
// the inherited _MTLDevice implementation to compile; keep the private dependency local to this adapter.
@interface NSObject (SweetCompiledLibraryData)
- (NSData *)libraryDataContents;
@end

static NSError *nbError(NSString *why);

static id<MTLLibrary> bridgeCompiledLibrary(id device,id<MTLLibrary> nativeLibrary,NSError **error) {
 if(!nativeLibrary)return nil;
 if(![nativeLibrary respondsToSelector:@selector(libraryDataContents)]) {
  if(error)*error=nbError(@"the system compiler did not expose compiled metallib data");
  return nil;
 }
 NSData *bytes=[(NSObject *)nativeLibrary libraryDataContents];
 if(!bytes.length) {
  if(error)*error=nbError(@"the system compiler returned an empty metallib");
  return nil;
 }
 NSData *retained=[bytes copy];
 dispatch_data_t data=dispatch_data_create(retained.bytes,retained.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)retained;});
 return [(id<MTLDevice>)device newLibraryWithData:data error:error];
}

static struct objc_super nativeDeviceSuper(id device) {
 struct objc_super superInfo={device,class_getSuperclass(object_getClass(device))};
 return superInfo;
}

static NSError *nbError(NSString *why) {
 return [NSError errorWithDomain:@"org.blu-sharky.native-metal" code:-1 userInfo:@{NSLocalizedDescriptionKey:why?:@"refused"}];
}
static NSString *lastError() {return [NSString stringWithUTF8String:nmb_last_error()];}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static void journal(const std::string &line) {
 if(gJournalPath.empty())return;
 std::lock_guard<std::mutex> lock(gJournalLock);
 if(FILE *f=fopen(gJournalPath.c_str(),"a")){fwrite(line.data(),1,line.size(),f);fputc('\n',f);fclose(f);}
}

@interface NBObject : NSObject
@property(atomic,copy) NSString *label;
@end
@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
// Metal SPI (Metal Performance Shaders asks every encoder and command buffer): an identity for its traces.
- (uint64_t)globalTraceObjectID {return (uint64_t)(uintptr_t)self;}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing(class_getName(object_getClass(self)),sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing %s %s",class_getName(object_getClass(self)),sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI \
- (uint64_t)protectionOptions {return 0;} \
- (id<MTLHeap>)heap {return _parentHeap;} \
- (BOOL)isAliasable {return NO;} \
/* 0208: MPS releases its temporary images' heap memory early with this (mpsprobe's blur: "-[NBTexture makeAliasable]: \
   unrecognized selector"). Aliasing only lets later heap resources reuse the memory; the runtime's heap has no reuse, \
   so the memory stays this resource's until it is freed, which Metal allows. isAliasable stays NO: true would \
   tell callers the contents may already be overwritten. */ \
- (void)makeAliasable {} \
- (int)setOwnerWithIdentity:(unsigned)identity {(void)identity;return 46; /* KERN_NOT_SUPPORTED */} \
- (void)waitUntilComplete {waitForResource(self);}
@interface NBBuffer : NBObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic,copy) void (^deallocator)(void *,NSUInteger);
@property(nonatomic) void *wrapped;
@property(nonatomic) IOSurfaceRef surfaceOwner; // keeps a shared import's CPU mapping alive
@property(nonatomic,strong) id<MTLHeap> parentHeap;   // the heap that made the buffer, nil otherwise
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBTexture : NBObject
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,strong) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset,rowBytes;
@property(nonatomic,strong) id<MTLTexture> parent;
@property(nonatomic,strong) id<MTLHeap> parentHeap;
@property(nonatomic) NSUInteger heapOffset;
@end
@implementation NBBuffer
- (void)dealloc {
 const NSUInteger length=nmb_buffer_length(_handle);nmb_buffer_release(_handle);
 if(_deallocator)_deallocator(_wrapped,length);
 if(_surfaceOwner)CFRelease(_surfaceOwner);
}
- (NSUInteger)length {return nmb_buffer_length(_handle);}
- (void *)contents {return nmb_buffer_contents(_handle);}
- (uint64_t)gpuAddress {return nmb_buffer_gpu_address(_handle);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (void)didModifyRange:(NSRange)range {(void)range;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_buffer_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: buffer setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize {return nmb_buffer_length(_handle);}
- (IOSurfaceRef)iosurface {return nullptr;}   // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
static NBBuffer *surfaceBuffer(IOSurfaceRef surface,void *base,size_t bytes) {
 struct Key {uint64_t identity,address,bytes;};
 const Key value={IOSurfaceGetID(surface),(uintptr_t)base,bytes};
 NSValue *key=value.identity?[NSValue valueWithBytes:&value objCType:@encode(Key)]:nil;
 std::lock_guard<std::mutex> lock(gSurfaceImportLock);
 if(!gSurfaceImports)gSurfaceImports=[NSMapTable strongToWeakObjectsMapTable];
 NBBuffer *buffer=key?[gSurfaceImports objectForKey:key]:nil;
 if(buffer)return buffer;
 NmbBuffer *handle=nmb_buffer_new_no_copy(gDevice,base,bytes,MTLResourceStorageModeShared);
 if(!handle){NBLOG(@"native-metal: surface import refused: %@",lastError());return nil;}
 buffer=[NBBuffer new];buffer.handle=handle;buffer.options=MTLResourceStorageModeShared;
 buffer.surfaceOwner=(IOSurfaceRef)CFRetain(surface);
 if(key) {
  // Bound metadata without retaining old frames. Evicting an association is
  // harmless: its owners still retain the actual import until their own end.
  if(gSurfaceImports.count>=256) {
   for(NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
    if(![gSurfaceImports objectForKey:old])[gSurfaceImports removeObjectForKey:old];
   if(gSurfaceImports.count>=256)[gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
  }
  [gSurfaceImports setObject:buffer forKey:key];
 }
 return buffer;
}
// guacamole: SweetSurface exposes the system compositor's private '_RB4' surface as one-plane, four-byte pixels. QuartzCore
// asks for RGB8_444_2P (542), but that Metal format describes two YCbCr-like planes and cannot import this backing. The
// _RB4 IOSurface has BGRA8 row storage, so treat that format as BGRA8 for textures and pipeline attachments together.
// The kernel also stores 10-bit wide-colour formats ('w30r', 'b3a8' with its alpha plane, ...) as one-plane BGRA8, which
// backboardd composites as BGRA8 (the panel is 8-bit sRGB). QuartzCore still asks the 10-bit XR pixel formats (550/551
// RGB10A8_2P_XR10, 552/553 BGRA10_XR, 554/555 BGR10_XR; 550 is private and metal2vulkan has none): every texture and
// every pipeline attachment of them is BGRA8Unorm(_sRGB), so they agree with each other
static uint64_t sweetNarrow(NSUInteger format) {
 if(format==542)return MTLPixelFormatBGRA8Unorm;
 return format>=550 && format<=555?((format&1)?MTLPixelFormatBGRA8Unorm_sRGB:MTLPixelFormatBGRA8Unorm):format;
}
static bool textureDescriptor(MTLTextureDescriptor *d,NmbTextureDescriptor *out) {
 if(!d)return false;
 *out={(uint64_t)d.textureType,sweetNarrow(d.pixelFormat),d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.resourceOptions};
 return true;
}
@implementation NBTexture
- (void)dealloc {nmb_texture_release(_handle);if(_surface)CFRelease(_surface);}
- (MTLTextureType)textureType {return (MTLTextureType)_descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)_descriptor.format;}
- (NSUInteger)width {return _descriptor.width;}
- (NSUInteger)height {return _descriptor.height;}
- (NSUInteger)depth {return _descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return _descriptor.levels;}
- (NSUInteger)sampleCount {return _descriptor.samples;}
- (NSUInteger)arrayLength {return _descriptor.array_length;}
- (MTLTextureUsage)usage {return (MTLTextureUsage)_descriptor.usage;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)_descriptor.options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_descriptor.options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_descriptor.options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (BOOL)isFramebufferOnly {return NO;}
- (IOSurfaceRef)iosurface {return _surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (id<MTLBuffer>)buffer {return _surface?nil:(id<MTLBuffer>)_buffer;}
- (NSUInteger)bufferOffset {return _offset;}
- (NSUInteger)bufferBytesPerRow {return _rowBytes;}
- (id<MTLTexture>)parentTexture {return _parent;}
- (NSUInteger)rotation {return 0;}
// 0204: Metal's private "is this a CAMetalLayer drawable's texture" (Core Image / Espresso in PhotosPosterProvider ask it
// while preparing a photo wallpaper: "-[NBTexture isDrawable]: unrecognized selector"). The drawables' textures are
// the ones CAMetalLayer labels so (as renderCommandEncoderWithDescriptor: tells them apart).
- (BOOL)isDrawable {return [self.label isEqualToString:@"CAMetalLayer Display Drawable"];}
// 0205: the memory a texture takes (MTLResource; PhotosPosterProvider asked next: "MISSING NBTexture -allocatedSize").
// Over a buffer/surface: its rows as laid out; otherwise texels x bytes a texel (by format) over the mip chain, times
// depth, slices and samples. An estimate where the runtime pads; what callers use it for is accounting.
static NSUInteger sweetBytesPerTexel(NSUInteger f) {
 if(f==250)return 2;                          // Depth16Unorm
 if(f==252||f==255)return 4;                  // Depth32Float, Depth24Unorm_Stencil8
 if(f==253)return 1;                          // Stencil8
 if(f==260)return 8;                          // Depth32Float_Stencil8
 if(f==552||f==553)return 8;                  // BGRA10_XR(_sRGB)
 if(f<20)return 1;                            // A8, R8
 if(f<50)return 2;                            // R16, RG8, 16-bit packed
 if(f<100)return 4;                           // R32, RG16, RGBA8/BGRA8, RGB10A2, RG11B10, RGB9E5
 if(f<120)return 8;                           // RG32, RGBA16
 if(f<130)return 16;                          // RGBA32
 if(f<250)return 1;                           // compressed (ASTC/ETC/PVRTC/BC): about a byte a texel
 return 4;
}
- (NSUInteger)allocatedSize {
 const NmbTextureDescriptor d=_descriptor;
 const NSUInteger slices=MAX((NSUInteger)d.array_length,(NSUInteger)1)*MAX((NSUInteger)d.samples,(NSUInteger)1)*
                         (d.type==5||d.type==6?6:1);           // cube / cube array: six faces
 if(_rowBytes && _buffer)return (NSUInteger)_rowBytes*MAX((NSUInteger)d.height,(NSUInteger)1)*MAX((NSUInteger)d.depth,(NSUInteger)1)*slices;
 const NSUInteger bpp=sweetBytesPerTexel((NSUInteger)d.format);
 NSUInteger total=0;
 for(NSUInteger l=0;l<MAX((NSUInteger)d.levels,(NSUInteger)1);l++)
  total+=MAX((NSUInteger)d.width>>l,(NSUInteger)1)*MAX((NSUInteger)d.height>>l,(NSUInteger)1)*MAX((NSUInteger)d.depth>>l,(NSUInteger)1)*bpp;
 return total*slices;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_texture_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: texture setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI
- (id<MTLTexture>)view:(NmbTexture *)handle {
 if(!handle){NBLOG(@"native-metal: texture view refused: %@",lastError());return nil;}
 NBTexture *view=[NBTexture new];view.handle=handle;NmbTextureDescriptor descriptor;nmb_texture_describe(handle,&descriptor);view.descriptor=descriptor;
 view.parent=(id<MTLTexture>)self;view.buffer=_buffer;view.offset=_offset;view.rowBytes=_rowBytes;
 if(_surface)view.surface=(IOSurfaceRef)CFRetain(_surface);
 return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
 return [self view:nmb_texture_new_view(_handle,format,0,nullptr,nullptr,true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view(_handle,format,type,l,s,false)];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_replace(_handle,region,level,slice,bytes,row,image,length))NBLOG(@"native-metal: replaceRegion refused: %@",lastError());
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_get_bytes(_handle,bytes,row,image,length,region,level,slice))NBLOG(@"native-metal: getBytes refused: %@",lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// A command can use many independent imports/views of one IOSurface. Their
// owners keep the mapping live through this phase. Maintain each exact CPU
// range once; no address identity or decision is carried into another phase
// or command, and distinct/partially overlapping ranges retain their calls.
// guacamole: a surface is cleaned only when the CPU wrote it since its last clean (IOSurface's seed moves on every unlock after a
// write lock) or never was: cleaning every surface of every command buffer (the 11 MB framebuffer, each app layer) took ~10% of
// backboardd's render thread (tsample, mtl2 run 11), and most are written by the GPU alone.
static std::unordered_map<uint32_t,uint32_t> gSurfaceSeeds;
static os_unfair_lock gSurfaceSeedsLock=OS_UNFAIR_LOCK_INIT;
static void flushSurfaces(NSMutableSet *surfaces) {
 if(!surfaces.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 ranges.reserve(surfaces.count);
 os_unfair_lock_lock(&gSurfaceSeedsLock);
 for(NBTexture *texture in surfaces)if(texture.surface) {
  const uint32_t id=IOSurfaceGetID(texture.surface),seed=IOSurfaceGetSeed(texture.surface);
  auto it=gSurfaceSeeds.find(id);
  if(it!=gSurfaceSeeds.end() && it->second==seed)continue;
  gSurfaceSeeds[id]=seed;
  ranges.emplace_back(reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(texture.surface)),IOSurfaceGetAllocSize(texture.surface));
 }
 if(gSurfaceSeeds.size()>4096)gSurfaceSeeds.clear();     // ids of surfaces long gone: clean them all once more
 os_unfair_lock_unlock(&gSurfaceSeedsLock);
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 gSurfaceFlushDuplicates+=(uint64_t)(ranges.end()-last);
 for(auto it=ranges.begin();it!=last;++it) {
  const uint64_t began=nowUs();
  sys_dcache_flush(reinterpret_cast<void *>(it->first),it->second);
  gSurfaceFlushBytes+=it->second;gTimeSurfaceFlush.add(began);
 }
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,MTLTextureDescriptor *d,NSUInteger offset,NSUInteger row,bool surface,
                                  bool planar=false,uint64_t surfacePlane=0,uint32_t surfacePixelFormat=0,uint64_t surfaceWidth=0,uint64_t surfaceHeight=0,
                                  uint64_t chromaOffset=0,uint64_t chromaRow=0) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_on_buffer(buffer.handle,&descriptor,offset,row,surface,planar,surfacePlane,surfacePixelFormat,surfaceWidth,surfaceHeight,chromaOffset,chromaRow);
 if(!handle){NBLOG(@"native-metal: texture over buffer refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 return (id<MTLTexture>)textureOnBuffer(self,d,offset,row,false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_linear(self.handle,&descriptor,offset,row,image);
 if(!handle){NBLOG(@"native-metal: linear texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=self;texture.offset=offset;texture.rowBytes=row;return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property(nonatomic) NmbLibrary *handle;
@end
@interface NBFunction : NBObject
@property(nonatomic) NmbFunction *handle;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) std::string constantsText;   // name:type:hex of each constant value it was specialized with (the journal's)
@end
@implementation NBFunction
- (void)dealloc {nmb_function_release(_handle);}
- (MTLFunctionOptions)options {return MTLFunctionOptionNone;}
- (MTLPatchType)patchType {return MTLPatchTypeNone;}
- (NSInteger)patchControlPointCount {return 0;}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
// Bytes of a function constant of MTLDataType `type`: lanes packed tightly, as metal2vulkan takes them (its CLI: bool3 = 3 bytes).
// 3-wide vectors added for iOS 16, whose QuartzCore specialises fixed_frag with uchar3 fc_color_opcodes0/1 (MTLDataTypeUChar3).
static uint64_t constantBytes(uint64_t type) {
 switch(type) {
  case 5:case 31:case 35:return 12;	/* float3, int3, uint3 */
  case 18:case 39:case 43:return 6;	/* half3, short3, ushort3 */
  case 47:case 51:case 55:return 3;	/* char3, uchar3, bool3 */
  case 83:case 87:return 24;		/* long3, ulong3 */
  case 3:case 29:case 33:return 4;
  case 4:case 30:case 34:return 8;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:return 2;
  case 17:case 38:case 42:return 4;
  case 19:case 40:case 44:return 8;
  case 45:case 49:case 53:return 1;
  case 46:case 50:case 54:return 2;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8;
  case 82:case 86:return 16;
  case 84:case 88:return 32;
  default:return 0;
 }
}
static std::string quartzCoreCallers();
@implementation NBLibrary
- (void)dealloc {nmb_library_release(_handle);}
// 0204: the same with a pipeline library (Metal SPI; MapKit's snapshot service: "MISSING NBLibrary
// -newFunctionWithName:constantValues:pipelineLibrary:error:", 43 times). The pipeline library is a cache of compiled
// pipelines: none is kept here, so the function is made as without one.
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values pipelineLibrary:(id)pipelineLibrary error:(NSError **)error {
 (void)pipelineLibrary;
 return [self newFunctionWithName:name constantValues:values error:error];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 if(!name || !values || !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8")) {
  NBLOG(@"native-metal: %@: unqualified function constant values (%s)",name,values?object_getClassName(values):"nil");
  if(error)*error=nbError(@"unqualified function constant values");return nil;
 }
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 std::vector<NmbConstant> constants;
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(!methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
     !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8")) {
   NBLOG(@"native-metal: %@: unqualified constant value ABI (%s)",name,object_getClassName(value));
   if(error)*error=nbError(@"unqualified constant value ABI");return nil;
  }
  id<NBOriginalConstantValue> v=value;
  const uint64_t bytes=constantBytes(v.dataType);
  if(!bytes || !v.data){NBLOG(@"native-metal: %@: constant type %lu is not qualified",name,(unsigned long)v.dataType);
   if(error)*error=nbError([NSString stringWithFormat:@"constant type %lu is not qualified",(unsigned long)v.dataType]);return nil;}
  constants.push_back({v.dataType,group?0:v.index,group?v.name.UTF8String:nullptr,v.data,bytes});
 }
 std::string text;
 for(const NmbConstant &c:constants) {
  char head[32];if(!c.name)snprintf(head,sizeof head,"#%llu",(unsigned long long)c.index);
  text+=text.empty()?"":" ";text+=c.name?c.name:head;snprintf(head,sizeof head,":%llu:",(unsigned long long)c.type);text+=head;
  for(uint64_t i=0;i<c.length;i++){snprintf(head,sizeof head,"%02x",((const uint8_t *)c.bytes)[i]);text+=head;}
 }
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=nmb_function_new_specialized(_handle,name.UTF8String,constants.data(),(uint32_t)constants.size());gTimeFunction.add(functionBegan);
 // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
 // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
 if(!gJournalPath.empty()) {
  char head[96];snprintf(head,sizeof head,"S t=%llu us=%llu ok=%d name=",(unsigned long long)((functionBegan-gJournalBegan)/1000),
      (unsigned long long)(nowUs()-functionBegan),handle?1:0);
  journal(head+std::string(name.UTF8String?:"?")+" k="+text+(handle?std::string():" err="+std::string(lastError().UTF8String?:"?"))+" | "+quartzCoreCallers());
 }
 if(!handle){
  noteProblem([NSString stringWithFormat:@"specialized function %@: %@",name,lastError()]);
  if(error)*error=nbError([NSString stringWithFormat:@"specialized function: %@",lastError()]);return nil;
 }
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 function.constantsText=text;if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:name constantValues:values error:&error];
 handler(function,error);
}
- (MTLLibraryType)type {return MTLLibraryTypeExecutable;}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {
 if(!d.name){if(error)*error=nbError(@"a function name is required");return nil;}
 if(d.constantValues)return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
 id<MTLFunction> function=[self newFunctionWithName:d.name];
 if(!function){if(error)*error=nbError([NSString stringWithFormat:@"function %@: %@",d.name,lastError()]);return nil;}
 if(error)*error=nil;return function;
}
- (NSArray<NSString *> *)functionNames {
 NSMutableArray *names=[NSMutableArray array];
 for(uint32_t i=0,n=nmb_library_function_count(_handle);i<n;i++) {
  char name[256];nmb_library_function_name(_handle,i,name,sizeof name);[names addObject:[NSString stringWithUTF8String:name]];
 }
 return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=name?nmb_function_new(_handle,name.UTF8String):nullptr;gTimeFunction.add(functionBegan);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
@end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc {nmb_compute_pipeline_release(_handle);}
- (NSUInteger)maxTotalThreadsPerThreadgroup {return nmb_compute_pipeline_max_threads(_handle);}
- (NSUInteger)threadExecutionWidth {return nmb_compute_pipeline_execution_width(_handle);}
@end
@interface NBRenderPipeline : NBObject
@property(nonatomic) NmbRenderPipeline *handle;
@end
@implementation NBRenderPipeline
- (void)dealloc {nmb_render_pipeline_release(_handle);}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))return 0;
 return s[0];
}
@end
@interface NBSampler : NBObject
@property(nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc {nmb_sampler_release(_handle);}
- (NSUInteger)pixelFormat {return MTLPixelFormatInvalid;}   // SPI: a sampler built from a plain descriptor names no format
@end
@interface NBDepthStencil : NBObject
@property(nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc {nmb_depth_stencil_release(_handle);}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property(nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc {nmb_event_release(_handle);}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context,uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue {return nmb_shared_event_value(self.handle);}
- (void)setSignaledValue:(uint64_t)value {nmb_shared_event_set_value(self.handle,value);}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {return nmb_shared_event_wait(self.handle,value,milliseconds);}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 if(!block)return;
 dispatch_queue_t queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);
 NSArray *job=@[self,queue,[block copy]];
 if(!nmb_shared_event_notify(self.handle,value,notified,(__bridge_retained void *)job)) {
  (void)(__bridge_transfer NSArray *)(__bridge void *)job;
  NBLOG(@"native-metal: notifyListener refused: %@",lastError());
 }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle {
 if(gMissing)gMissing("NBSharedEvent","newSharedEventHandle (cross-process)");
 return nil;
}
@end
static void notified(void *context,uint64_t value) {
 NSArray *job=(__bridge_transfer NSArray *)context;
 id<MTLSharedEvent> event=job[0];MTLSharedEventNotificationBlock block=job[2];
 dispatch_async(job[1],^{block(event,value);});
}
@interface NBFence : NBObject
@property(nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc {nmb_fence_release(_handle);}
@end

// ---- command buffers and encoders ----
@interface NBQueue : NBObject
@property(nonatomic) NmbQueue *handle;
@property(atomic,strong) dispatch_queue_t completionQueue;   // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end
@interface NBCommandBuffer : NBObject {
@public
 EncodedWork encodedWork;   // the command buffer's single encoder thread, not global counter deltas
 uint64_t createdUs;
 bool drawsToSurface;      // 0243: a render pass of this buffer writes an IOSurface (a CAMetalLayer drawable, say)
}
@property(nonatomic) NmbCommandBuffer *handle;
@property(nonatomic,strong) NBQueue *queue;
@property(atomic,copy) NSString *encodeFailure;
@property(atomic) BOOL committed,failedLocally;
@property(nonatomic) uint64_t committedUs;
@property(nonatomic) uint32_t commitIndex;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers,*completedHandlers;
@property(nonatomic,strong) NSMutableArray<id<MTLDrawable>> *presentDrawables;
@property(nonatomic,strong) NSArray *priorWorkBuffers;
@property(nonatomic,strong) NSMutableSet *surfaces;
@property(nonatomic,strong) NSMutableSet *rotatedShadows;   // 0196: landscape stand-ins rendered into (rotatedSurfaceTexture)
@property(atomic,strong) NSMutableDictionary *userDictionary;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property(nonatomic,strong) NSMutableSet *resourceOwners;
@property(nonatomic,strong) dispatch_group_t handlers;
@property(nonatomic,strong) dispatch_group_t schedulingHandlers;
@property(nonatomic) BOOL schedulingDelivered;
- (void)refuse:(NSString *)what;
@end
@interface NBEncoder : NBObject {
@public
 void *_lastOwner[2];   // 0242: the buffer / texture last added to the command buffer's owners (not retained: identity only)
}
@property(nonatomic) NmbEncoder *handle;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource) {
 const uint64_t began=nowUs();
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending) {
  int dependency=-1;
  if([resource isKindOfClass:[NBBuffer class]])dependency=nmb_command_buffer_uses_buffer(commandBuffer.handle,[(NBBuffer *)resource handle]);
  else if([resource isKindOfClass:[NBTexture class]])dependency=nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)resource handle]);
  gResourceChecks++;
  // An unknown result retains the old conservative wait. False means no matching storage
  // (or GPU work already completed), not a fabricated completion for a pending dependency.
  if(dependency!=0)[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];else gResourceSkipped++;
 }
 gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc {if(_handle)nmb_encoder_discard(_handle);}
- (bool)check:(bool)ok what:(const char *)what {
 if(!ok)[_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@",what,lastError()]];
 return ok;
}
- (void)endEncoding {
 NmbEncoder *handle=_handle;_handle=nullptr;
 if(!handle){[_commandBuffer refuse:@"endEncoding twice"];return;}
 [self check:nmb_encoder_end(handle) what:"endEncoding"];
}
- (void)insertDebugSignpost:(NSString *)s {(void)s;}
- (void)pushDebugGroup:(NSString *)s {(void)s;}
- (void)popDebugGroup {}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]){[_commandBuffer refuse:@"foreign buffer"];return;}
 if(buffer && (__bridge void *)buffer!=_lastOwner[0]){[_commandBuffer.resourceOwners addObject:buffer];_lastOwner[0]=(__bridge void *)buffer;}   // 0242
 [self check:nmb_encoder_set_buffer(_handle,stage,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index) what:"setBuffer"];
}
static void sweetYCbCrRefresh(id<MTLTexture> t);   // 0257b
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(texture && ![(id)texture isKindOfClass:[NBTexture class]]){[_commandBuffer refuse:@"foreign texture"];return;}
 if(texture && (__bridge void *)texture!=_lastOwner[1]){   // 0242: the same texture again: already owned
  [_commandBuffer.resourceOwners addObject:texture];_lastOwner[1]=(__bridge void *)texture;
  if([(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 }
 if(texture)sweetYCbCrRefresh(texture);   // 0257b
 [self check:nmb_encoder_set_texture(_handle,stage,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index) what:"setTexture"];
 if(!traceOn())return;
 // 0188: a watched surface, any stage: the encoder, its pipeline (render) and target, where it is bound
 if(texture && watchedSurface([(NBTexture *)texture surface]) && gWatchLogs.fetch_add(1)<80){
  IOSurfaceRef sf=[(NBTexture *)texture surface];
  NBLOG(@"native-metal: watched surface %#x bound: %@ stage %u index %lu view %lux%lu fmt %lu type %lu | pipe %@ | target %@",IOSurfaceGetID(sf),
   NSStringFromClass([self class]),stage,(unsigned long)index,(unsigned long)texture.width,(unsigned long)texture.height,(unsigned long)texture.pixelFormat,
   (unsigned long)texture.textureType,objc_getAssociatedObject(self,"pipe")?:@"-",objc_getAssociatedObject(self,"target")?:@"-");
 }
 // 0182: a full-screen surface bound for the fragment stage (another process's picture in backboardd's compositions)
 if(stage==2){
  NSString *key=[NSString stringWithFormat:@"ftex%lu",(unsigned long)index];
  IOSurfaceRef sf=texture?[(NBTexture *)texture surface]:nullptr;
  NSString *v=(sf && IOSurfaceGetWidth(sf)*IOSurfaceGetHeight(sf)>=2000000 /* 0187: landscape too */)?
   [NSString stringWithFormat:@"t%lu=surf %#x %lux%lu fmt %lu",(unsigned long)index,IOSurfaceGetID(sf),(unsigned long)texture.width,(unsigned long)texture.height,(unsigned long)texture.pixelFormat]:nil;
  // 0219: 0217's converted YCbCr pictures too (the photo wallpaper's '420f' images in backboardd): their draws are traced
  if(!v && texture)if(NSString *yc=objc_getAssociatedObject(texture,"ycbcr"))v=[NSString stringWithFormat:@"t%lu=%@",(unsigned long)index,yc];
  objc_setAssociatedObject(self,(const void *)NSSelectorFromString(key),v,OBJC_ASSOCIATION_RETAIN);
  NSMutableDictionary *big=objc_getAssociatedObject(self,"bigtex");
  if(!big){big=[NSMutableDictionary dictionary];objc_setAssociatedObject(self,"bigtex",big,OBJC_ASSOCIATION_RETAIN);}
  if(v)big[@(index)]=v;else [big removeObjectForKey:@(index)];
 }
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler && ![(id)sampler isKindOfClass:[NBSampler class]]){[_commandBuffer refuse:@"foreign sampler"];return;}
 [self check:nmb_encoder_set_sampler(_handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index) what:"setSamplerState"];
 if(stage==2 && traceOn()){   // 0182 (0242: tracing only)
  NSMutableDictionary *smp=objc_getAssociatedObject(self,"smp");
  if(!smp){smp=[NSMutableDictionary dictionary];objc_setAssociatedObject(self,"smp",smp,OBJC_ASSOCIATION_RETAIN);}
  NSString *dsc=sampler?objc_getAssociatedObject(sampler,"desc"):nil;
  if(dsc)smp[@(index)]=dsc;else [smp removeObjectForKey:@(index)];
 }
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 if(![(id)fence isKindOfClass:[NBFence class]]){[_commandBuffer refuse:@"foreign fence"];return;}
 NmbFence *handle=[(NBFence *)fence handle];
 [self check:update?nmb_encoder_update_fence(_handle,handle,stages):nmb_encoder_wait_fence(_handle,handle,stages) what:update?"updateFence":"waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:true];}
- (void)waitForFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:false];}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_buffer_offset(_handle,stage,offset,(uint32_t)index) what:"setBufferOffset"];
}
@end

@interface NBComputeEncoder : NBEncoder
@end
@implementation NBComputeEncoder
- (MTLDispatchType)dispatchType {return MTLDispatchTypeSerial;}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(![(id)state isKindOfClass:[NBComputePipeline class]]){[self.commandBuffer refuse:@"foreign compute pipeline"];return;}
 [self check:nmb_compute_set_pipeline(self.handle,[(NBComputePipeline *)state handle]) what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:0];}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:0];}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:0];}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:0];}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:0];}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i {
 [self check:nmb_compute_set_threadgroup_memory(self.handle,(uint32_t)n,(uint32_t)i) what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads {
 gDispatches++;self.commandBuffer->encodedWork.dispatches++;
 const uint64_t s[3]={size.width,size.height,size.depth},g[3]={group.width,group.height,group.depth};
 [self check:nmb_compute_dispatch(self.handle,s,g,threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:true];}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:false];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {(void)r;(void)u;}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {(void)s;}
@end

@interface NBRenderEncoder : NBEncoder {
@public
 // 0242: cull mode, winding and fill mode as plain fields (were associated NSNumbers, set on nearly every draw by
 // Unity) and sent to the runtime only when one changes. Metal's defaults: none, clockwise, fill.
 uint64_t _cull,_winding,_fill;
 bool _rasterSent;
}
@property(nonatomic) BOOL drawableTarget;
@end
// 0182: draws that sample a full-screen surface, with pipeline, samplers and target: each distinct combination once (<= 60)
static void traceBigTextureDraw(id encoder,const char *kind,NSUInteger count,NSUInteger instances) {
 if(!traceOn())return;   // 0242
 NSDictionary *big=objc_getAssociatedObject(encoder,"bigtex");
 if(!big.count)return;
 static std::mutex lock;static NSMutableSet<NSString *> *seen;
 NSString *pipe=objc_getAssociatedObject(encoder,"pipe")?:@"(pipeline ?)";
 NSDictionary *smp=objc_getAssociatedObject(encoder,"smp");
 NSString *target=objc_getAssociatedObject(encoder,"target")?:@"?";
 NSString *line=[NSString stringWithFormat:@"%@ | %@ | samplers %@ | target %@",
  [[big allValues] componentsJoinedByString:@","],pipe,[[smp allValues] componentsJoinedByString:@" / "],target];
 std::lock_guard<std::mutex> guard(lock);if(!seen)seen=[NSMutableSet set];
 if([seen containsObject:line] || seen.count>=60)return;
 [seen addObject:line];
 NBLOG(@"native-metal: big-texture draw (%s %lu x%lu): %@",kind,(unsigned long)count,(unsigned long)instances,line);
}
@implementation NBRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(![(id)state isKindOfClass:[NBRenderPipeline class]]){[self.commandBuffer refuse:@"foreign render pipeline"];return;}
 [self check:nmb_render_set_pipeline(self.handle,[(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
 if(traceOn())objc_setAssociatedObject(self,"pipe",objc_getAssociatedObject(state,"desc"),OBJC_ASSOCIATION_RETAIN);   // 0182 (0242: tracing only)
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:1];}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:2];}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:1];}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:2];}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:1];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:1];}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:2];}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:1];}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:2];}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:2];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:2];}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range {
 for(NSUInteger k=0;k<range.length;k++)[self sampler:samplers[k] index:range.location+k stage:1];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range {
 for(NSUInteger k=0;k<range.length;k++)[self sampler:samplers[k] index:range.location+k stage:2];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[NBDepthStencil class]]){[self.commandBuffer refuse:@"foreign depth/stencil state"];return;}
 [self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];
}
- (void)setViewport:(MTLViewport)v {
 const double values[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};
 [self check:nmb_render_set_viewport(self.handle,values) what:"setViewport"];
}
- (void)setScissorRect:(MTLScissorRect)r {
 const uint64_t values[4]={r.x,r.y,r.width,r.height};
 [self check:nmb_render_set_scissor(self.handle,values) what:"setScissorRect"];
}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults until each is set.
- (void)raster {
 [self check:nmb_render_set_raster(self.handle,_cull,_winding,_fill) what:"raster state"];
 _rasterSent=true;
}
- (void)setCullMode:(MTLCullMode)m {if(_rasterSent && _cull==(uint64_t)m)return;_cull=m;[self raster];}
// 0204: depth clip mode (Find My: "MISSING NBRenderEncoder -setDepthClipMode:"). Clip is Metal's default and what the
// runtime does; clamp (depth clamping) is not passed on, logged the first time it is asked for.
- (void)setDepthClipMode:(MTLDepthClipMode)mode {
 if(mode==MTLDepthClipModeClamp){static std::atomic<bool> said{false};
  if(!said.exchange(true))NBLOG(@"native-metal: depth clamp asked for (setDepthClipMode:), drawn with clipping");}
}
- (void)setFrontFacingWinding:(MTLWinding)w {if(_rasterSent && _winding==(uint64_t)w)return;_winding=w;[self raster];}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f {if(_rasterSent && _fill==(uint64_t)f)return;_fill=f;[self raster];}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
 [self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];
}
- (void)setLineWidth:(float)width {[self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
 [self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];
}
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index {
 MTLStoreAction effective=action;
 const uint64_t logIndex=++gLateStoreActionLogs;
 if(self.drawableTarget && index==0 && action==MTLStoreActionUnknown){
  effective=MTLStoreActionStore;
  const uint64_t overrideIndex=++gLateStoreOverrides;
  if(overrideIndex<=3 || overrideIndex%120==0)NBLOG(@"native-metal: late drawable store Unknown overridden to Store #%llu",(unsigned long long)overrideIndex);
 }
 if(logIndex<=3 || logIndex%120==0)NBLOG(@"native-metal: setColorStoreAction index=%lu requested=%lu effective=%lu drawable=%d",(unsigned long)index,(unsigned long)action,(unsigned long)effective,(int)self.drawableTarget);
 [self check:nmb_render_set_color_store_action(self.handle,effective,(uint32_t)index) what:"setColorStoreAction"];
}
- (void)setDepthStoreAction:(MTLStoreAction)action {
 [self check:nmb_render_set_depth_store_action(self.handle,action) what:"setDepthStoreAction"];
}
- (void)setStencilStoreAction:(MTLStoreAction)action {
 [self check:nmb_render_set_stencil_store_action(self.handle,action) what:"setStencilStoreAction"];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 [self texture:t index:ti stage:2];[self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth {return 0;}
- (NSUInteger)tileHeight {return 0;}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {(void)t;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {(void)p;(void)n;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size {(void)size;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region {(void)size;(void)region;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setStencilReferenceValue:(uint32_t)v {[self check:nmb_render_set_stencil_reference(self.handle,v,v) what:"setStencilReferenceValue"];}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b {
 [self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
 const float c[4]={r,g,b,a};[self check:nmb_render_set_blend_color(self.handle,c) what:"setBlendColor"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 gDraws++;self.commandBuffer->encodedWork.draws++;
 traceBigTextureDraw(self,"draw",n,i);
 [self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(![(id)b isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:b];
 gDraws++;self.commandBuffer->encodedWork.draws++;
 traceBigTextureDraw(self,"indexed",n,i);
 [self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {(void)r;(void)u;(void)s;}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:true];}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:false];}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++)if(![objects[i] isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"missing or foreign blit buffer"];return false;}
 for(size_t i=0;i<count;i++)[self.commandBuffer.resourceOwners addObject:objects[i]];
 return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++) {
  id o=objects[i];
  if(![o isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"missing or foreign blit texture"];return false;}
  [self.commandBuffer.resourceOwners addObject:o];
  if([(NBTexture *)o surface])[self.commandBuffer.surfaces addObject:o];
 }
 return true;
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture {
 if(![self textures:NB_OBJECTS(texture) count:1])return;
 // Turnip has no separate texture residency optimization step; Metal's operation is a no-op here.
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {
 (void)slice;(void)level;
 if(![self textures:NB_OBJECTS(texture) count:1])return;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)dO size:(NSUInteger)n {
 if(![self buffers:NB_OBJECTS(s,d) count:2])return;
 [self check:nmb_blit_copy_buffer(self.handle,[(NBBuffer *)s handle],so,[(NBBuffer *)d handle],dO,n) what:"copyFromBuffer:toBuffer:"];
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 if(![self buffers:NB_OBJECTS(b) count:1])return;
 [self check:nmb_blit_fill_buffer(self.handle,[(NBBuffer *)b handle],r.location,r.location+r.length,v) what:"fillBuffer"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 if((watchedSurface([(NBTexture *)s surface]) || watchedSurface([(NBTexture *)d surface])) && gWatchLogs.fetch_add(1)<80)   // 0188
  NBLOG(@"native-metal: watched surface blit: %#x %lux%lu fmt %lu -> %#x %lux%lu fmt %lu, origin %lu,%lu size %lux%lu",
   [(NBTexture *)s surface]?IOSurfaceGetID([(NBTexture *)s surface]):0,(unsigned long)s.width,(unsigned long)s.height,(unsigned long)s.pixelFormat,
   [(NBTexture *)d surface]?IOSurfaceGetID([(NBTexture *)d surface]):0,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,
   (unsigned long)so.x,(unsigned long)so.y,(unsigned long)z.width,(unsigned long)z.height);
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_copy_texture(self.handle,[(NBTexture *)s handle],ss,sl,a,size,[(NBTexture *)d handle],ds,dl,b) what:"copyFromTexture:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const NSUInteger levels=MIN(s.mipmapLevelCount,d.mipmapLevelCount),slices=MIN(s.arrayLength,d.arrayLength);
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  MTLSize size=MTLSizeMake(MAX(s.width>>level,(NSUInteger)1),MAX(s.height>>level,(NSUInteger)1),MAX(s.depth>>level,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d destinationSlice:slice
       destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO {
 if(![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])return;
 const uint64_t size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_buffer_to_texture(self.handle,[(NBBuffer *)s handle],o,row,image,size,[(NBTexture *)d handle],slice,level,b) what:"copyFromBuffer:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
 if(![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth};
 [self check:nmb_blit_texture_to_buffer(self.handle,[(NBTexture *)s handle],slice,level,a,size,[(NBBuffer *)d handle],o,row,image) what:"copyFromTexture:toBuffer:"];
}
// 0186: the forms with MTLBlitOption (Unity 6 / Angry Birds 2 calls the buffer-to-texture one on its first frame; the
// missing selector ended in a crash). None is the plain copy. The depth/stencil-aspect options choose which half of a
// combined depth/stencil texture is copied; the bridge copies the texture as it is, so those are passed through with
// a note (once each). PVRTC row layout does not occur here (no PVRTC formats are offered).
static void blitOptionNote(MTLBlitOption options,const char *what) {
 if(options==MTLBlitOptionNone)return;
 static std::atomic<uint32_t> said{0};
 const uint32_t bit=1u<<((options&7u)%8u);
 if(said.fetch_or(bit)&bit)return;
 NBLOG(@"native-metal: %s with MTLBlitOption %lu: copied without the option (aspect selection not supported)",what,(unsigned long)options);
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO options:(MTLBlitOption)options {
 blitOptionNote(options,"copyFromBuffer:toTexture:");
 [self copyFromBuffer:s sourceOffset:o sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:z toTexture:d destinationSlice:slice
     destinationLevel:level destinationOrigin:dO];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image
                options:(MTLBlitOption)options {
 blitOptionNote(options,"copyFromTexture:toBuffer:");
 [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:so sourceSize:z toBuffer:d destinationOffset:o
  destinationBytesPerRow:row destinationBytesPerImage:image];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(![self textures:NB_OBJECTS(t) count:1])return;
 [self check:nmb_blit_generate_mipmaps(self.handle,[(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}
- (void)synchronizeResource:(id<MTLResource>)r {(void)r;}
@end

// 0196: CoreAnimation's framebuffer while a landscape app is in front. backboardd asks for its portrait framebuffer
// surfaces (1125x2436, 4608 bytes a row) as 2436x1125 textures: Apple's driver gives a texture whose texels land rotated
// in the surface. The runtime cannot (texture over buffer refused: RowPitch 4608 < 9744), so CA had nowhere to draw
// and the screen kept its last portrait picture (Angry Birds Classic, AB2). Instead it gets a landscape texture of our
// own (the "shadow", which keeps what CA drew, so loads and partial redraws work); every command buffer that rendered
// into it ends with one full-screen pass that turns it into the surface (a portrait texture over the surface).
// Direction: counter-clockwise by default (a landscape image's top row goes to the surface's left column; clockwise
// showed Angry Birds Classic upside down); `touch /var/local/sweet-rotate-cw` turns it the other way,
// `touch /var/local/sweet-norotate` refuses as before. Both are read when a shadow is made (after a respring).
static NSString *const kRotationSource=@"#include <metal_stdlib>\n"
 "using namespace metal;\n"
 "struct RotV { float4 position [[position]]; };\n"
 "vertex RotV rot_v(uint vid [[vertex_id]]) {\n"
 " float2 p=float2(float((vid<<1)&2),float(vid&2)); RotV o; o.position=float4(p*2.0-1.0,0.0,1.0); return o;\n"
 "}\n"
 // portrait pixel p (width W = the landscape height, height H = the landscape width)
 "fragment float4 rot_cw(RotV in [[stage_in]], texture2d<float, access::read> src [[texture(0)]]) {\n"
 " uint2 p=uint2(in.position.xy); uint W=src.get_height(); return src.read(uint2(p.y, W-1-min(p.x,W-1)));\n"
 "}\n"
 "fragment float4 rot_ccw(RotV in [[stage_in]], texture2d<float, access::read> src [[texture(0)]]) {\n"
 " uint2 p=uint2(in.position.xy); uint H=src.get_width(); return src.read(uint2(H-1-min(p.y,H-1), p.x));\n"
 "}\n";
static std::atomic<uint64_t> gRotations{0};
static id<MTLRenderPipelineState> rotationPipeline(MTLPixelFormat format,unsigned mode) {
 static std::mutex lock;static NSMutableDictionary *pipelines;static id<MTLLibrary> library;static bool failed;
 std::lock_guard<std::mutex> guard(lock);
 if(!pipelines)pipelines=[NSMutableDictionary dictionary];
 NSNumber *key=@((uint64_t)format*2+mode);
 if(id state=pipelines[key])return state;
 if(failed)return nil;
 NSError *error=nil;
 if(!library){
  library=[(id<MTLDevice>)gDeviceObject newLibraryWithSource:kRotationSource options:nil error:&error];
  if(!library){failed=true;NBLOG(@"native-metal: rotated framebuffer: the turning shader does not compile: %@",error);return nil;}
 }
 MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
 d.label=@"sweet rotated framebuffer";
 d.vertexFunction=[library newFunctionWithName:@"rot_v"];
 d.fragmentFunction=[library newFunctionWithName:mode?@"rot_ccw":@"rot_cw"];
 d.colorAttachments[0].pixelFormat=format;
 id<MTLRenderPipelineState> state=d.vertexFunction && d.fragmentFunction?
  [(id<MTLDevice>)gDeviceObject newRenderPipelineStateWithDescriptor:d error:&error]:nil;
 if(!state){NBLOG(@"native-metal: rotated framebuffer: no pipeline for format %lu: %@",(unsigned long)format,error);return nil;}
 pipelines[key]=state;
 return state;
}
// The shadow a texture is, or is a view of (nil: none).
static NBTexture *rotationShadowOf(id t) {
 for(int depth=0;t && depth<4;depth++){
  if(![t isKindOfClass:[NBTexture class]])return nil;
  if(objc_getAssociatedObject(t,"rotPortrait"))return (NBTexture *)t;
  t=[(NBTexture *)t parent];
 }
 return nil;
}
static id<MTLTexture> rotatedSurfaceTexture(id device,MTLTextureDescriptor *d,IOSurfaceRef surface) {
 if(access("/var/local/sweet-norotate",F_OK)==0)return nil;
 // counter-clockwise by default: clockwise showed Angry Birds Classic upside down (user, 2026-11-09)
 const unsigned mode=access("/var/local/sweet-rotate-cw",F_OK)==0?0:1;
 long spi=-1;
 if([d respondsToSelector:NSSelectorFromString(@"rotation")]){@try{spi=[[d valueForKey:@"rotation"] longValue];}@catch(NSException *e){}}
 static std::mutex lock;static NSMutableDictionary *shadows;static std::atomic<uint32_t> said{0};
 std::lock_guard<std::mutex> guard(lock);
 if(!shadows)shadows=[NSMutableDictionary dictionary];
 NSString *key=[NSString stringWithFormat:@"%u:%lu:%lux%lu",IOSurfaceGetID(surface),(unsigned long)d.pixelFormat,(unsigned long)d.width,(unsigned long)d.height];
 if(NBTexture *shadow=shadows[key]){
  NBTexture *portrait=objc_getAssociatedObject(shadow,"rotPortrait");
  if(portrait && IOSurfaceGetBaseAddress(portrait.surface)==IOSurfaceGetBaseAddress(surface) &&
     [objc_getAssociatedObject(shadow,"rotMode") unsignedIntValue]==mode)return shadow;
  [shadows removeObjectForKey:key];
 }
 if(!rotationPipeline(d.pixelFormat,mode))return nil;
 MTLTextureDescriptor *pd=[d copy];pd.width=d.height;pd.height=d.width;pd.usage=d.usage|MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> portrait=[(id<MTLDevice>)device newTextureWithDescriptor:pd iosurface:surface plane:0];
 if(![(id)portrait isKindOfClass:[NBTexture class]]){NBLOG(@"native-metal: rotated framebuffer: surface %#x refused even the right way up",IOSurfaceGetID(surface));return nil;}
 MTLTextureDescriptor *sd=[d copy];sd.storageMode=MTLStorageModePrivate;sd.usage=d.usage|MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> made=[(id<MTLDevice>)device newTextureWithDescriptor:sd];
 if(![(id)made isKindOfClass:[NBTexture class]]){NBLOG(@"native-metal: rotated framebuffer: no %lux%lu texture of our own",(unsigned long)d.width,(unsigned long)d.height);return nil;}
 NBTexture *shadow=(NBTexture *)made;
 shadow.surface=(IOSurfaceRef)CFRetain(surface);    // what CA asked for: -iosurface answers it
 objc_setAssociatedObject(shadow,"rotPortrait",portrait,OBJC_ASSOCIATION_RETAIN);
 objc_setAssociatedObject(shadow,"rotMode",@(mode),OBJC_ASSOCIATION_RETAIN);
 if(shadows.count>=8)[shadows removeAllObjects];
 shadows[key]=shadow;
 if(said.fetch_add(1)<16)
  NBLOG(@"native-metal: rotated framebuffer: surface %#x %zux%zu asked as %lux%lu fmt %lu usage %lu: a landscape texture of ours, turned %s into the surface (descriptor rotation %ld)",
   IOSurfaceGetID(surface),IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),(unsigned long)d.width,(unsigned long)d.height,
   (unsigned long)d.pixelFormat,(unsigned long)d.usage,mode?"counter-clockwise":"clockwise",spi);
 return shadow;
}
static void completed(void *context);
static void scheduled(void *context);
@implementation NBCommandBuffer
- (instancetype)init {
 if((self=[super init])){_scheduledHandlers=[NSMutableArray array];_completedHandlers=[NSMutableArray array];_presentDrawables=[NSMutableArray array];_surfaces=[NSMutableSet set];_rotatedShadows=[NSMutableSet set];_userDictionary=[NSMutableDictionary dictionary];_resourceOwners=[NSMutableSet set];_handlers=dispatch_group_create();_schedulingHandlers=dispatch_group_create();}
 if(self)createdUs=nowUs();
 return self;
}
- (void)dealloc {nmb_command_buffer_release(_handle);}
- (id<MTLCommandQueue>)commandQueue {return (id<MTLCommandQueue>)_queue;}
- (BOOL)retainedReferences {return YES;}
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions {return 0;}
- (void)setProtectionOptions:(uint64_t)options {if(options)[self refuse:[NSString stringWithFormat:@"protection options 0x%llx",options]];}
- (void)refuse:(NSString *)what {
 if(!self.encodeFailure){self.encodeFailure=what;NBLOG(@"native-metal: command buffer refused: %@",what);}
}
- (MTLCommandBufferStatus)status {
 if(self.failedLocally)return MTLCommandBufferStatusError;
 return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error {
 if(self.failedLocally)return nbError(self.encodeFailure);
 if(self.status!=MTLCommandBufferStatusError)return nil;
 char text[512]={};nmb_command_buffer_error(_handle,text,sizeof text);
 return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name {(void)name;}
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime {return 0;}
- (CFTimeInterval)kernelEndTime {return 0;}
- (BOOL)commitAndWaitUntilSubmitted {[(id<MTLCommandBuffer>)self commit];[self waitUntilScheduled];return !self.failedLocally;}
- (CFTimeInterval)GPUStartTime {return nmb_command_buffer_gpu_time(_handle,false);}
- (CFTimeInterval)GPUEndTime {return nmb_command_buffer_gpu_time(_handle,true);}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what {
 if(self.committed){[self refuse:[what stringByAppendingString:@" after commit"]];return nil;}
 if(!handle){[self refuse:[NSString stringWithFormat:@"%@: %@",what,lastError()]];return nil;}
 encoder.handle=handle;encoder.commandBuffer=self;return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 return [self encoder:[NBComputeEncoder new] handle:self.committed?nullptr:nmb_compute_encoder_new(_handle) what:@"computeCommandEncoder"];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
 if(type!=MTLDispatchTypeSerial){[self refuse:@"concurrent compute dispatch"];return nil;}
 return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 gBlits++;encodedWork.blits++;
 return [self encoder:[NBBlitEncoder new] handle:self.committed?nullptr:nmb_blit_encoder_new(_handle) what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 NmbRenderPassDescriptor pass={};
 BOOL drawableTarget=NO;
 {
  id<MTLTexture> first=d.colorAttachments[0].texture;
  if(first) {
   drawableTarget=[first.label isEqualToString:@"CAMetalLayer Display Drawable"];
   const uint64_t pixels=(uint64_t)first.width*first.height;
   gPasses++;gPassPixels+=pixels;encodedWork.passes++;encodedWork.pixels+=pixels;
   const uint64_t passIndex=++gPassDetailLogs;
   if(passIndex<=3 || passIndex%30==0)NBLOG(@"native-metal: render target #%llu %lux%lu format=%lu load=%lu store=%lu label=%@",(unsigned long long)passIndex,(unsigned long)first.width,(unsigned long)first.height,(unsigned long)first.pixelFormat,(unsigned long)d.colorAttachments[0].loadAction,(unsigned long)d.colorAttachments[0].storeAction,first.label?:@"");
   uint64_t seen=gBiggestPass.load();while(pixels>seen && !gBiggestPass.compare_exchange_weak(seen,pixels)){}
   if(pixels>=2000000)gFullScreenPasses++;
  }
 }
 auto attachment=[&](MTLRenderPassAttachmentDescriptor *a,NmbPassAttachment &out)->bool {
  if(!a.texture)return true;
  if(![(id)a.texture isKindOfClass:[NBTexture class]])return false;
  NBTexture *texture=(NBTexture *)a.texture;if(texture.surface){[_surfaces addObject:texture];drawsToSurface=true;}
  if(NBTexture *shadow=rotationShadowOf(texture))[_rotatedShadows addObject:shadow];   // 0196
  [_resourceOwners addObject:texture];
  out.texture=texture.handle;out.level=a.level;out.slice=a.slice;out.depth_plane=a.depthPlane;out.load=a.loadAction;out.store=a.storeAction;
  // 0247: the multisample resolve target (Maps: 4x MSAA resolved into its CAMetalLayer drawable, which stayed black)
  if(id<MTLTexture> r=a.resolveTexture){
   if(![(id)r isKindOfClass:[NBTexture class]])return false;
   NBTexture *resolve=(NBTexture *)r;if(resolve.surface){[_surfaces addObject:resolve];drawsToSurface=true;}
   if(NBTexture *shadow=rotationShadowOf(resolve))[_rotatedShadows addObject:shadow];
   [_resourceOwners addObject:resolve];
   out.resolve=resolve.handle;out.resolve_level=a.resolveLevel;out.resolve_slice=a.resolveSlice;out.resolve_depth_plane=a.resolveDepthPlane;
   static std::atomic<uint64_t> resolves{0};const uint64_t k=++resolves;
   if(k<=3 || k%300==0)NBLOG(@"native-metal: pass resolves %lux%lu (%lu samples) into %lux%lu fmt %lu surf %#x #%llu (0247)",(unsigned long)texture.width,
    (unsigned long)texture.height,(unsigned long)texture.sampleCount,(unsigned long)r.width,(unsigned long)r.height,(unsigned long)r.pixelFormat,
    resolve.surface?IOSurfaceGetID(resolve.surface):0,(unsigned long long)k);
  }
  return true;
 };
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];
  if(!attachment(c,pass.colors[i])){[self refuse:@"foreign color attachment"];return nil;}
  if(c.texture && c.storeAction==MTLStoreActionUnknown &&
     [c.texture.label isEqualToString:@"CAMetalLayer Display Drawable"]){
   pass.colors[i].store=MTLStoreActionStore;
   const uint64_t overrideIndex=++gDrawableStoreOverrides;
   if(overrideIndex<=3 || overrideIndex%120==0)NBLOG(@"native-metal: CAMetalLayer drawable store Unknown overridden to Store #%llu attachment=%u",(unsigned long long)overrideIndex,i);
  }
  pass.colors[i].clear[0]=c.clearColor.red;pass.colors[i].clear[1]=c.clearColor.green;pass.colors[i].clear[2]=c.clearColor.blue;pass.colors[i].clear[3]=c.clearColor.alpha;
 }
 if(!attachment(d.depthAttachment,pass.depth) || !attachment(d.stencilAttachment,pass.stencil)){[self refuse:@"foreign depth/stencil attachment"];return nil;}
 pass.depth.clear[0]=d.depthAttachment.clearDepth;pass.stencil.clear[0]=d.stencilAttachment.clearStencil;
 pass.width=d.renderTargetWidth;pass.height=d.renderTargetHeight;pass.array_length=d.renderTargetArrayLength;pass.samples=d.defaultRasterSampleCount;
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]){[self refuse:@"foreign visibility result buffer"];return nil;}
  [_resourceOwners addObject:d.visibilityResultBuffer];
  pass.visibility=[(NBBuffer *)d.visibilityResultBuffer handle];
 }
 id encoder=[self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
 if([encoder isKindOfClass:[NBRenderEncoder class]])((NBRenderEncoder *)encoder).drawableTarget=drawableTarget;
 if(traceOn() && [encoder isKindOfClass:[NBRenderEncoder class]]){   // 0182: the pass's colour target, for the draw trace (0242: tracing only)
  id<MTLTexture> t0=d.colorAttachments[0].texture;IOSurfaceRef sf=[(id)t0 isKindOfClass:[NBTexture class]]?[(NBTexture *)t0 surface]:nullptr;
  objc_setAssociatedObject(encoder,"target",[NSString stringWithFormat:@"%lux%lu fmt %lu surf %#x load %lu label %@",(unsigned long)t0.width,(unsigned long)t0.height,
   (unsigned long)t0.pixelFormat,sf?IOSurfaceGetID(sf):0,(unsigned long)d.colorAttachments[0].loadAction,t0.label?:@""],OBJC_ASSOCIATION_RETAIN);
 }
 return encoder;
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_scheduledHandlers addObject:[block copy]];}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_completedHandlers addObject:[block copy]];}}
- (void)presentDrawable:(id<MTLDrawable>)drawable {
 if(!drawable || ![drawable respondsToSelector:@selector(present)]){[self refuse:@"presentDrawable: invalid drawable"];return;}
 id<MTLTexture> texture=((id<CAMetalDrawable>)drawable).texture;
 const uint64_t queued=++gDrawablePresentQueued;
 if(queued<=3 || queued%120==0) {
  const uint32_t surface=[(id)texture isKindOfClass:[NBTexture class]] && ((NBTexture *)texture).surface ? IOSurfaceGetID(((NBTexture *)texture).surface) : 0;
  NBLOG(@"native-metal: drawable queued #%llu class=%@ texture=%@ %lux%lu IOSurface=%u",(unsigned long long)queued,NSStringFromClass([(id)drawable class]),NSStringFromClass([(id)texture class]),(unsigned long)texture.width,(unsigned long)texture.height,surface);
 }
 @synchronized(self){
  if(self.committed){[self refuse:@"presentDrawable: after commit"];return;}
  [_presentDrawables addObject:drawable];
 }
}
- (void)enqueue {}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal {
 if(![(id)event isKindOfClass:[NBEvent class]]){[self refuse:@"foreign event"];return;}
 if(self.committed){[self refuse:@"event encoded after commit"];return;}
 NmbEvent *handle=[(NBEvent *)event handle];
 if(!(signal?nmb_command_buffer_signal_event(_handle,handle,value):nmb_command_buffer_wait_event(_handle,handle,value)))
  [self refuse:[NSString stringWithFormat:@"%s: %@",signal?"encodeSignalEvent":"encodeWaitForEvent",lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:true];}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:false];}
- (void)didSchedule {
 {
  const uint64_t took=nowUs()-self.committedUs;
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[self.commitIndex%32];
  if(r.index==self.commitIndex)r.scheduledUs=took;
 }
 NSArray *handlers;
 @synchronized(self){
  _schedulingDelivered=YES;handlers=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_schedulingHandlers);
 };
 // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
 // deadlock checks. The queue's completionQueue routes completed handlers only.
 run();
}
// After completion (or a refusal): invalidate the CPU's view of imported surfaces, then completed handlers, once.
- (void)finish {
 flushSurfaces(_surfaces);
 NSArray<id<MTLDrawable>> *drawables;
 @synchronized(self){drawables=[_presentDrawables copy];[_presentDrawables removeAllObjects];}
 const MTLCommandBufferStatus status=self.status;
 if(status==MTLCommandBufferStatusCompleted)for(id<MTLDrawable> drawable in drawables){
  @try {
   // The Metal bridge's Vulkan fence is not visible to CoreAnimation. Do not
   // publish a drawable until Turnip has finished writing its IOSurface.
   id<MTLDrawable> retainedDrawable=drawable;
   const uint32_t commitIndex=self.commitIndex;
   const BOOL hasWork=encodedWork.passes || encodedWork.draws || encodedWork.dispatches || encodedWork.blits;
   NSArray<NBCommandBuffer *> *priorWorkBuffers=self.priorWorkBuffers;
   dispatch_async(drawablePresentQueue(),^{
    @try {
     if(!hasWork){
      const uint64_t waitBegan=nowUs();
      for(NBCommandBuffer *workBuffer in priorWorkBuffers)[workBuffer waitUntilCompleted];
      const uint64_t waitUs=nowUs()-waitBegan;
      const uint64_t waited=++gPresentPriorWorkWaits;
      if(waited<=3 || waited%120==0)NBLOG(@"native-metal: present-only prior work fences complete #%llu commit=%u buffers=%lu wait=%llu us",(unsigned long long)waited,commitIndex,(unsigned long)priorWorkBuffers.count,(unsigned long long)waitUs);
     }
     [retainedDrawable addPresentedHandler:^(id<MTLDrawable> presented){
      const uint64_t count=++gDrawableActuallyPresented;
      if(count<=3 || count%120==0)NBLOG(@"native-metal: CoreAnimation presented #%llu drawableID=%lu time=%.6f",(unsigned long long)count,(unsigned long)presented.drawableID,presented.presentedTime);
     }];
     {
      // 0173: what CoreAnimation is about to be given (diagnostic, sweetmtlblack=1)
      if(blackSamplingEnabled()){
       const unsigned black=prePresentBlackSamples(((id<CAMetalDrawable>)retainedDrawable).texture);
       const uint64_t frames=++gPrePresentFrames;
       if(black>=16){
        const uint64_t b=++gPrePresentBlack;
        if(b<=20)NBLOG(@"native-metal: pre-present black frame #%llu commit=%u %u/64 black",(unsigned long long)b,commitIndex,black);
       }
       if(frames%120==0)NBLOG(@"native-metal: pre-present totals: %llu frames, %llu black",(unsigned long long)frames,(unsigned long long)gPrePresentBlack.load());
      }
     }
     [retainedDrawable present];
     const uint64_t issued=++gDrawablePresentIssued;
     if(issued<=3 || issued%120==0)NBLOG(@"native-metal: drawable presented after Vulkan completion #%llu commit=%u work=%d",(unsigned long long)issued,commitIndex,(int)hasWork);
    } @catch(NSException *exception){NBLOG(@"native-metal: completed drawable present exception: %@",exception.reason);}
   });
   const uint64_t sampleIndex=++gDrawableSamples;
   id<MTLTexture> texture=((id<CAMetalDrawable>)drawable).texture;
   const MTLPixelFormat format=texture.pixelFormat;
   if(blackSamplingEnabled() && [texture isKindOfClass:[NBTexture class]] &&
      (format==MTLPixelFormatBGRA8Unorm || format==MTLPixelFormatRGBA8Unorm ||
       format==MTLPixelFormatBGRA8Unorm_sRGB || format==MTLPixelFormatRGBA8Unorm_sRGB) && texture.width>=8 && texture.height>=8) {
    const NSUInteger xs[5]={texture.width/4,3*texture.width/4,texture.width/2,texture.width/4,3*texture.width/4};
    const NSUInteger ys[5]={texture.height/4,texture.height/4,texture.height/2,3*texture.height/4,3*texture.height/4};
    unsigned nonblack[5]={};uint64_t means[5]={};
    for(unsigned regionIndex=0;regionIndex<5;regionIndex++){
     uint8_t pixels[8*8*4]={};
     const MTLRegion region=MTLRegionMake2D(xs[regionIndex]-4,ys[regionIndex]-4,8,8);
     [texture getBytes:pixels bytesPerRow:8*4 bytesPerImage:8*8*4 fromRegion:region mipmapLevel:0 slice:0];
     uint64_t sum=0;
     for(unsigned p=0;p<64;p++){const unsigned rgb=pixels[p*4]+pixels[p*4+1]+pixels[p*4+2];sum+=rgb;if(rgb>24)nonblack[regionIndex]++;}
     means[regionIndex]=sum/(64*3);
    }
    if(sampleIndex<=3 || sampleIndex%30==0){
     NBLOG(@"native-metal: frame grid #%llu commit=%u work=%u/%u/%u/%u pixels=%llu nonblack=%u,%u,%u,%u,%u mean=%llu,%llu,%llu,%llu,%llu",(unsigned long long)sampleIndex,self.commitIndex,self->encodedWork.draws,self->encodedWork.passes,self->encodedWork.dispatches,self->encodedWork.blits,(unsigned long long)(self->encodedWork.pixels>>10),nonblack[0],nonblack[1],nonblack[2],nonblack[3],nonblack[4],(unsigned long long)means[0],(unsigned long long)means[1],(unsigned long long)means[2],(unsigned long long)means[3],(unsigned long long)means[4]);
    }
    {std::lock_guard<std::mutex> lock(gDrawableSampleLock);for(unsigned i=0;i<5;i++)if(!nonblack[i])gBlackRegionSamples[i]++;}
    if(sampleIndex%120==0){
     std::lock_guard<std::mutex> lock(gDrawableSampleLock);
     NBLOG(@"native-metal: black-region totals at #%llu TL,TR,C,BL,BR=%llu,%llu,%llu,%llu,%llu",(unsigned long long)sampleIndex,(unsigned long long)gBlackRegionSamples[0],(unsigned long long)gBlackRegionSamples[1],(unsigned long long)gBlackRegionSamples[2],(unsigned long long)gBlackRegionSamples[3],(unsigned long long)gBlackRegionSamples[4]);
    }
   } else if(blackSamplingEnabled() && (sampleIndex<=3 || sampleIndex%30==0)) {
    NBLOG(@"native-metal: frame grid #%llu skipped format=%lu texture=%@",(unsigned long long)sampleIndex,(unsigned long)format,NSStringFromClass([(id)texture class]));
   }
  } @catch(NSException *exception){NBLOG(@"native-metal: drawable present exception: %@",exception.reason);}
 } else if(drawables.count) {
  const uint64_t skipped=++gDrawablePresentSkipped;
  NBLOG(@"native-metal: drawable present skipped #%llu command status=%u",(unsigned long long)skipped,(unsigned)status);
 }
 // Actual GPU completion (or a local refusal that never submitted) is the
 // release point. A retained completed command must not pin every old frame.
 [_surfaces removeAllObjects];[_resourceOwners removeAllObjects];
 @synchronized(gInFlight){[gInFlight removeObject:self];}
 NSArray *completedHandlers;
 @synchronized(self){
  // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
  if(!_schedulingDelivered){_schedulingDelivered=YES;[_scheduledHandlers removeAllObjects];dispatch_group_leave(_schedulingHandlers);}
  completedHandlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in completedHandlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_handlers);
 };
 dispatch_queue_t queue=_queue.completionQueue;
 if(queue)dispatch_async(queue,run);else run();
}
- (void)commit {
 if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 if(_rotatedShadows.count && !self.encodeFailure){   // 0196: what CA drew in landscape, into the portrait surface
  NSArray *shadows=[_rotatedShadows allObjects];[_rotatedShadows removeAllObjects];
  for(NBTexture *shadow in shadows){
   NBTexture *portrait=objc_getAssociatedObject(shadow,"rotPortrait");
   id<MTLRenderPipelineState> state=rotationPipeline(shadow.pixelFormat,[objc_getAssociatedObject(shadow,"rotMode") unsignedIntValue]);
   if(!portrait || !state)continue;
   MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
   rp.colorAttachments[0].texture=portrait;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
   id<MTLRenderCommandEncoder> encoder=[(id<MTLCommandBuffer>)self renderCommandEncoderWithDescriptor:rp];
   if(!encoder)break;
   [encoder setRenderPipelineState:state];
   [encoder setFragmentTexture:shadow atIndex:0];
   [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [encoder endEncoding];
   const uint64_t n=++gRotations;
   if(n<=3 || n%600==0)NBLOG(@"native-metal: rotated framebuffer: turn %llu into surface %#x",(unsigned long long)n,IOSurfaceGetID(portrait.surface));
  }
 }
 self.committed=YES;
 self.committedUs=nowUs();
 dispatch_group_enter(_handlers);
 dispatch_group_enter(_schedulingHandlers);
 @synchronized(gInFlight){
  // Serialize commit indices with publication to gInFlight. A presentation-only buffer
  // captures every earlier GPU-work submission still outstanding, including work on
  // another logical Metal command queue in this process.
  {
   std::lock_guard<std::mutex> lock(gCommitLock);
   const uint32_t index=(uint32_t)(gCommits.fetch_add(1)+1);self.commitIndex=index;
   CommitRecord &r=gCommitRing[index%32];
   r={};r.index=index;r.committedUs=self.committedUs;r.ageUs=self.committedUs-createdUs;r.work=encodedWork;
   if(encodedWork.passes || encodedWork.draws || encodedWork.dispatches || encodedWork.blits){
    const uint64_t workIndex=++gWorkCommits;
    if(workIndex<=3 || workIndex%30==0){
     NSUInteger drawableCount=0;@synchronized(self){drawableCount=_presentDrawables.count;}
     NBLOG(@"native-metal: work commit #%llu index=%u drawables=%lu draws=%u passes=%u dispatches=%u blits=%u pixels=%llu",(unsigned long long)workIndex,index,(unsigned long)drawableCount,encodedWork.draws,encodedWork.passes,encodedWork.dispatches,encodedWork.blits,(unsigned long long)(encodedWork.pixels>>10));
    }
   }
  }
  if(!encodedWork.passes && !encodedWork.draws && !encodedWork.dispatches && !encodedWork.blits){
   NSMutableArray *prior=[NSMutableArray array];
   for(NBCommandBuffer *buffer in gInFlight){
    const BOOL hasWork=buffer->encodedWork.passes || buffer->encodedWork.draws || buffer->encodedWork.dispatches || buffer->encodedWork.blits;
    if(buffer.commitIndex<self.commitIndex && hasWork)[prior addObject:buffer];
   }
   self.priorWorkBuffers=[prior copy];
  }
  [gInFlight addObject:self];
 }
 if(!self.encodeFailure) {
  flushSurfaces(_surfaces);
  // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
  void *context=(__bridge_retained void *)self;
  const bool callbacks=nmb_command_buffer_on_completed(_handle,completed,context) &&
                       nmb_command_buffer_on_scheduled(_handle,scheduled,context);
  if(callbacks){
   // decided before the commit: completion (another thread) empties _surfaces
   const BOOL syncWait=_surfaces.count && syncSubmitEnabled();
   if(nmb_command_buffer_commit(_handle)){
    if(syncWait){
     // runtime-level wait only: no dispatch-group wait on scheduled handlers from CoreAnimation's render thread
     (void)nmb_command_buffer_wait_scheduled(_handle);
     const uint64_t n=++gSyncSubmitWaits;
     if(n<=3 || n%600==0)NBLOG(@"native-metal: sync submit #%llu waited %llu us",(unsigned long long)n,(unsigned long long)(nowUs()-self.committedUs));
    }
    return;
   }
  }
  (void)CFBridgingRelease(context);   // refused registration/commit cannot call the registered handlers
  [self refuse:[@"commit: " stringByAppendingString:lastError()]];
 }
 self.failedLocally=YES;gCommitFailures++;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{[self finish];});
}
- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
 gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)){
  NBLOG(@"native-metal: waitUntilScheduled refused: %@",lastError());return;
 }
 dispatch_group_wait(_schedulingHandlers,DISPATCH_TIME_FOREVER);
 /*
  * 0243: CAMetalLayer.presentsWithTransaction (Maps' VectorKit): the app commits, waits until scheduled and calls
  * [drawable present] itself, so CoreAnimation composites the drawable's IOSurface at the next transaction. On
  * Apple's GPU "scheduled" means the work is ordered ahead of the window server's; our Vulkan fence is invisible to
  * CoreAnimation, so the window server read the surface before Turnip had drawn it: Maps' map stayed black (it
  * redraws only when something changes). A buffer that renders into an IOSurface now waits for its GPU work here
  * (the GPU only, not the completed handlers, which may need this very thread). Not in backboardd, whose own
  * frames are shown through the kernel's swap, which already waits for the GPU.
  */
 static const bool windowServer=strcmp(getprogname(),"backboardd")==0;
 if(drawsToSurface && !windowServer && !self.failedLocally){
  nmb_command_buffer_wait(_handle,0);
  static std::atomic<uint64_t> waits{0};const uint64_t w=++waits;
  if(w<=3 || w%300==0)NBLOG(@"native-metal: waitUntilScheduled on a surface-drawing buffer waited for the GPU #%llu (0243)",(unsigned long long)w);
 }
 gTimeWaitScheduled.add(began);
}
@end
static void scheduled(void *context) {[(__bridge NBCommandBuffer *)context didSchedule];}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context) {
 gCompleted++;
 NBCommandBuffer *commandBuffer=(__bridge_transfer NBCommandBuffer *)context;
 {
  const uint64_t took=nowUs()-commandBuffer.committedUs;
  gTimeEncodeToCommit.count++;gTimeEncodeToCommit.total+=took;
  uint64_t seen=gTimeEncodeToCommit.slowest.load();while(took>seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen,took)){}
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[commandBuffer.commitIndex%32];
  if(r.index==commandBuffer.commitIndex)r.callbackUs=took;
 }
 [commandBuffer finish];
}

@implementation NBQueue
- (void)dealloc {nmb_queue_release(_handle);}
- (id<MTLCommandBuffer>)commandBuffer {
 NmbCommandBuffer *handle=nmb_command_buffer_new(_handle);
 if(!handle){NBLOG(@"native-metal: commandBuffer refused: %@",lastError());return nil;}
 NBCommandBuffer *commandBuffer=[NBCommandBuffer new];commandBuffer.handle=handle;commandBuffer.queue=self;return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {(void)queue;}   // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// GPU priorities are not applied (one hardware queue): reported as not applied.
- (BOOL)setGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {(void)d;return [self commandBuffer];}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error,NSString *what,T value) {
 noteProblem([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 if(error)*error=nbError([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers() {
 /* SweetMetal2 (iOS 16.7): only the return addresses into QuartzCore, as offsets from its header. UniXNU's version also read
  * MetalContext fields and debug bytes at iPadOS 17.7.11 (21H461) offsets, which mean nothing here. */
 void *frames[24];const int n=backtrace(frames,24);
 std::string out="qc";char item[32];unsigned kept=0;
 for(int i=0;i<n && kept<8;i++) {
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname || !strstr(info.dli_fname,"/QuartzCore"))continue;
  snprintf(item,sizeof item," %lx",(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));out+=item;++kept;
 }
 return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
 uint64_t user=0,system=0;int64_t faults=0,pageins=0,switches=0;
 static NBCost now() {
  NBCost c;thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
  if(thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS) {
   c.user=(uint64_t)t.user_time.seconds*1000000+t.user_time.microseconds;c.system=(uint64_t)t.system_time.seconds*1000000+t.system_time.microseconds;
  }
  mach_port_deallocate(mach_task_self(),self);
  task_events_info_data_t e{};n=TASK_EVENTS_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS){c.faults=e.faults;c.pageins=e.pageins;c.switches=e.csw;}
  return c;
 }
 std::string since(const NBCost &b) const {
  char text[128];snprintf(text,sizeof text," cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",(unsigned long long)(user+system-b.user-b.system),
      (unsigned long long)(system-b.system),(long long)(faults-b.faults),(long long)(pageins-b.pageins),(long long)(switches-b.switches));
  return text;
 }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d,uint64_t began,const char *refused,const std::string &cost) {
 if(gJournalPath.empty())return;
 const uint64_t ended=nowUs();uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
 char head[256];
 snprintf(head,sizeof head,"R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",(unsigned long long)((began-gJournalBegan)/1000),
     (unsigned long long)tid,pthread_main_np(),(unsigned long long)(ended-began),refused?0:1,(unsigned long)d.rasterSampleCount,
     (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat);
 std::string line=head;
 line+=cost;
 line+=" label=";line+=d.label.length?d.label.UTF8String:"-";
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  snprintf(head,sizeof head," c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)sweetNarrow(a.pixelFormat),(unsigned long)a.writeMask,(int)a.blendingEnabled,
      (unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,(unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,
      (unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation);
  line+=head;
 }
 if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex<31?a.bufferIndex:0];
  snprintf(head,sizeof head," a%u=%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex,(unsigned long)l.stride,
      (unsigned long)l.stepFunction,(unsigned long)l.stepRate);
  line+=head;
 }
 NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
 NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
 line+=" | v ";line+=vf?vf.name.UTF8String:"-";line+=" ";line+=vf?vf.constantsText:"";
 line+=" | f ";line+=ff?ff.name.UTF8String:"-";line+=" ";line+=ff?ff.constantsText:"";
 if(refused){line+=" | refused ";line+=refused;}
 line+=" | ";line+=quartzCoreCallers();
 for(char &c:line)if(c=='\n')c=' ';
 journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d,NmbRenderPipelineDescriptor *p,NSError **error) {
 if(![(id)d.vertexFunction isKindOfClass:[NBFunction class]] || (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
  if(error)*error=nbError(@"foreign function");return false;
 }
 p->vertex=[(NBFunction *)d.vertexFunction handle];p->fragment=d.fragmentFunction?[(NBFunction *)d.fragmentFunction handle]:nullptr;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  p->colors[i]={sweetNarrow(a.pixelFormat),(uint64_t)a.writeMask,(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 p->depth_format=d.depthAttachmentPixelFormat;p->stencil_format=d.stencilAttachmentPixelFormat;p->samples=d.rasterSampleCount;
 p->topology=d.inputPrimitiveTopology;p->rasterization=d.rasterizationEnabled;
 if(MTLVertexDescriptor *v=d.vertexDescriptor) {
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
   p->vertex_inputs=1;p->attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
   if(a.bufferIndex<31){MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex];p->layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};}
  }
 }
 return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode.
@interface NBHeap : NBObject
@property(nonatomic) NmbHeap *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) MTLHeapType heapType;
@end
// 0211: a full automatic heap's resources come from the device (log the first few, and any refusal).
static void sweetHeapOverflowNote(bool made,NSString *why) {
 static std::atomic<unsigned> notes{0};
 if(!made)NBLOG(@"native-metal: heap full (%@) and the device refused the resource too: %@",why,lastError());
 else if(notes.fetch_add(1)<3)NBLOG(@"native-metal: heap full (%@): resource made outside the heap",why);
}
@implementation NBHeap
- (void)dealloc {nmb_heap_release(_handle);}
- (NSUInteger)size {return nmb_heap_size(_handle);}
- (NSUInteger)usedSize {return nmb_heap_used_size(_handle);}
- (NSUInteger)currentAllocatedSize {return nmb_heap_allocated_size(_handle);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {return nmb_heap_max_available_size(_handle,alignment);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (MTLHeapType)type {return _heapType;}
- (uint64_t)protectionOptions {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_heap_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: heap setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset {
 NmbBuffer *handle=nmb_heap_new_buffer(_handle,length,options,offset);
 // 0211: an automatic heap that is full takes the resource from the device instead (see textureWithDescriptor:offset:).
 if(!handle && offset==UINT64_MAX){
  NSString *why=lastError();
  handle=nmb_buffer_new(gDevice,length,options,nullptr);
  sweetHeapOverflowNote(handle!=nullptr,why);
 }
 if(!handle){NBLOG(@"native-metal: heap buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.parentHeap=(id<MTLHeap>)self;
 buffer.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {return [self bufferWithLength:length options:options offset:UINT64_MAX];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
 return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_heap_new_texture(_handle,&descriptor,offset);
 // 0211: PhotosPosterProvider's MPS work filled its heaps ("heap texture refused: OutOfMemory(Heap)", 16 in a row, and
 // the photo wallpaper came out black). MPS sizes a heap counting on makeAliasable to free each temporary image's
 // memory for the next one, and this runtime's heaps never reuse memory (0208: makeAliasable is a no-op). For an
 // automatic heap (no offset asked) the texture comes from the device instead: callers see an ordinary heap texture
 // (its heap is this one), only its memory is outside. Placement heaps (an offset) keep the refusal: their caller
 // laid out the memory and asked for that spot.
 if(!handle && offset==UINT64_MAX){
  NSString *why=lastError();
  handle=nmb_texture_new(gDevice,&descriptor);
  sweetHeapOverflowNote(handle!=nullptr,why);
 }
 if(!handle){NBLOG(@"native-metal: heap texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.parentHeap=(id<MTLHeap>)self;texture.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {return [self textureWithDescriptor:d offset:UINT64_MAX];}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset {return [self textureWithDescriptor:d offset:offset];}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property(nonatomic,strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
static bool computeDescriptor(MTLComputePipelineDescriptor *d,NmbComputePipelineDescriptor *p,NSError **error) {
 if(!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign compute function");return false;}
 // Metal may materialize empty stage/linked descriptors on access. Preserve the
 // default meaning while refusing state this frontend cannot yet represent.
 MTLStageInputOutputDescriptor *stage=d.stageInputDescriptor;
 bool empty=!stage || (stage.indexType==MTLIndexTypeUInt16 && stage.indexBufferIndex==0);
 for(NSUInteger index=0;stage && index<31;++index) {
  MTLAttributeDescriptor *a=stage.attributes[index];MTLBufferLayoutDescriptor *b=stage.layouts[index];
  empty=empty && a.format==MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
        b.stepFunction==MTLStepFunctionPerVertex && b.stepRate==1;
 }
 if(!empty){if(error)*error=nbError(@"compute stage inputs are not implemented");return false;}
 MTLLinkedFunctions *linked=d.linkedFunctions;
 if(linked.functions.count || linked.binaryFunctions.count || linked.privateFunctions.count || linked.groups.count ||
    d.preloadedLibraries.count || d.insertLibraries.count) {
  if(error)*error=nbError(@"compute linked/dynamic libraries are not implemented");return false;
 }
 p->function=[(NBFunction *)d.computeFunction handle];p->max_threads=d.maxTotalThreadsPerThreadgroup;
 p->multiple_of_execution_width=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;p->indirect=d.supportIndirectCommandBuffers;
 p->max_call_stack_depth=d.maxCallStackDepth;p->adding_binary_functions=d.supportAddingBinaryFunctions;
 for(NSUInteger index=0;index<31;++index)p->mutability[index]=d.buffers[index].mutability;
 return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property(nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc {nmb_binary_archive_release(_handle);}
- (NSUInteger)options {return 0;}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 auto *p=new NmbRenderPipelineDescriptor();
 const bool ok=renderDescriptor(d,p,error) && nmb_binary_archive_add_render(_handle,p);delete p;
 if(!ok)return failWith<BOOL>(error,@"archive render pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return NO;
 if(!nmb_binary_archive_add_compute_descriptor(_handle,&p))return failWith<BOOL>(error,@"archive compute pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return NO;}
 if(!nmb_binary_archive_serialize(_handle,url.fileSystemRepresentation))return failWith<BOOL>(error,@"archive serialize",NO);
 if(error)*error=nil;return YES;
}
@end

static bool pipelineArchives(NSArray *list,std::vector<NmbBinaryArchive *> &archives,NSError **error) {
 for(id archive in list) {
  if(![archive isKindOfClass:[NBBinaryArchive class]]){if(error)*error=nbError(@"foreign binary archive");return false;}
  archives.push_back([(NBBinaryArchive *)archive handle]);
 }
 return true;
}

@interface NBDeviceMethods : NSObject
@end
// Bytes of one texel of the uncompressed formats Core Animation and RenderBox use (4 for the rest).
static uint64_t sweetTexelBytes(MTLPixelFormat f) {
 switch((NSUInteger)f) {
 case 1: case 10: case 11: case 12: case 13: case 14: return 1;
 case 20: case 22: case 23: case 24: case 25: case 30: case 32: case 33: case 34: case 40: case 41: case 42: case 43: return 2;
 case 103: case 104: case 105: case 110: case 111: case 112: case 113: case 114: case 115: case 552: case 553: return 8;
 case 123: case 124: case 125: return 16;
 default: return 4;
 }
}
typedef struct {uint64_t w[2];} SweetWatermark;   /* AGXTextureLayout's {?=[2Q]} watermark */
// Block footprint and bytes of the compressed formats the A640 samples (ASTC LDR/sRGB, ETC2/EAC); false for the rest.
static bool sweetBlock(MTLPixelFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
 static const uint8_t astc[][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{0,0},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 const NSUInteger n = (NSUInteger)f;
 if((n >= 186 && n <= 200) || (n >= 204 && n <= 218)) {
  const NSUInteger i = n >= 204 ? n - 204 : n - 186;
  if(!astc[i][0]) return false;
  *bw = astc[i][0]; *bh = astc[i][1]; *bytes = 16; return true;
 }
 switch(n) {
 case 170: case 172: case 180: case 181: case 182: case 183: *bw = 4; *bh = 4; *bytes = 8; return true;   /* EAC R11, ETC2 RGB8(A1) */
 case 174: case 176: case 178: case 179: *bw = 4; *bh = 4; *bytes = 16; return true;                          /* EAC RG11, RGBA8 */
 default: return false;
 }
}
// Keeps a no-copy texture's memory owner until the texture goes (its deallocator runs then).
@interface SweetMemoryOwner : NSObject
@property(nonatomic,copy) void (^deallocator)(void *,NSUInteger);
@property(nonatomic) void *bytes;
@property(nonatomic) NSUInteger length;
@end
@implementation SweetMemoryOwner
- (void)dealloc {if(_deallocator)_deallocator(_bytes,_length);}
@end
static char sweetMemoryOwnerKey;
// SweetMetal2's MTLTextureLayout (Metal SPI: AGXTextureLayout's protocol on Apple's GPUs): a linear layout of `row`-byte rows.
@interface SweetTextureLayout : NSObject
- (instancetype)initWithRow:(uint64_t)row height:(uint64_t)height texel:(uint64_t)texel;
- (instancetype)initWithBlocks:(uint32_t)bw by:(uint32_t)bh bytes:(uint32_t)bytes width:(uint64_t)width height:(uint64_t)height;
@end
@implementation SweetTextureLayout {
 uint64_t _row,_height,_texel;
 uint32_t _bw,_bh;     /* block footprint (1x1 uncompressed); _height and _texel count block rows and block bytes */
}
- (instancetype)initWithRow:(uint64_t)row height:(uint64_t)height texel:(uint64_t)texel {
 if((self=[super init])){_row=row;_height=height;_texel=texel?texel:4;_bw=_bh=1;}
 return self;
}
- (instancetype)initWithBlocks:(uint32_t)bw by:(uint32_t)bh bytes:(uint32_t)bytes width:(uint64_t)width height:(uint64_t)height {
 if((self=[super init])){_bw=bw;_bh=bh;_texel=bytes;_row=(width+bw-1)/bw*bytes;_height=(height+bh-1)/bh;}
 return self;
}
- (NSUInteger)size {const uint64_t page=(uint64_t)getpagesize();return (NSUInteger)((_row*_height+page-1)&~(page-1));}
- (NSUInteger)alignment {return (NSUInteger)getpagesize();}
- (MTLSize)copyGranularity {return MTLSizeMake(_bw,_bh,1);}
- (SweetWatermark)watermark {SweetWatermark none={{0,0}};return none;}
- (void)initializeTextureMemory:(void *)memory {(void)memory;}
- (void)finalizeTextureMemory:(void *)memory {(void)memory;}
- (void)copyFromLinearBytes:(const void *)src linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)srcRow linearBytesPerImage:(NSUInteger)image
            toTextureMemory:(void *)dst textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level textureRegion:(MTLRegion)r {
 (void)image;if(slice || level)return;
 const NSUInteger x0=r.origin.x/_bw,y0=r.origin.y/_bh,w=(r.size.width+_bw-1)/_bw,h=(r.size.height+_bh-1)/_bh;
 for(NSUInteger y=0;y<h && y0+y<_height;y++)
  memcpy((uint8_t *)dst+(y0+y)*_row+x0*_texel,(const uint8_t *)src+offset+y*srcRow,w*_texel);
}
- (void)copyFromTextureMemory:(const void *)src textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level textureRegion:(MTLRegion)r
              toLinearBytes:(void *)dst linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)dstRow linearBytesPerImage:(NSUInteger)image {
 (void)image;if(slice || level)return;
 const NSUInteger x0=r.origin.x/_bw,y0=r.origin.y/_bh,w=(r.size.width+_bw-1)/_bw,h=(r.size.height+_bh-1)/_bh;
 for(NSUInteger y=0;y<h && y0+y<_height;y++)
  memcpy((uint8_t *)dst+offset+y*dstRow,(const uint8_t *)src+(y0+y)*_row+x0*_texel,w*_texel);
}
@end
// 0217: see newTextureWithDescriptor:iosurface:plane:. nil when the surface is not as expected (then the runtime's own refusal stands).
// 0257b: converts a two-plane 4:2:0 surface into an RGBA8 texture of ow x oh (same math as sweetYCbCrTexture). For video
// frames it is run again at every bind (sweetYCbCrRefresh): CoreAnimation makes one texture per IOSurface and reuses it,
// counting on a live import, but ours is a converted copy, so a pooled decoder surface showed the frame it held when
// first seen (Safari mp4: the right frame mixed with the first ones and middle ones).
static bool sweetYCbCrFill(id<MTLTexture> t,IOSurfaceRef surface,size_t ow,size_t oh) {
 const size_t w=IOSurfaceGetWidthOfPlane(surface,0),h=IOSurfaceGetHeightOfPlane(surface,0);
 const size_t cw=IOSurfaceGetWidthOfPlane(surface,1),ch=IOSurfaceGetHeightOfPlane(surface,1);
 if(!w || !h || ow>w || oh>h || cw<(w+1)/2 || ch<(h+1)/2)return false;
 size_t yrow=IOSurfaceGetBytesPerRowOfPlane(surface,0),crow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
 if(!yrow)yrow=(w+63)&~(size_t)63;
 if(!crow)crow=(cw*2+63)&~(size_t)63;
 const uint8_t *y=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0),*c=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
 if(!y || !c)return false;
 const uint32_t pf=IOSurfaceGetPixelFormat(surface);
 const bool video=pf=='420v' || pf=='x420';
 std::vector<uint8_t> rgba(ow*oh*4);
 for(size_t j=0;j<oh;j++){
  const uint8_t *yr=y+yrow*j,*cr=c+crow*(j/2);uint8_t *o=rgba.data()+ow*4*j;
  for(size_t i=0;i<ow;i++){
   int yy=yr[i],cb=cr[(i/2)*2]-128,cv=cr[(i/2)*2+1]-128;
   if(video){yy=(yy-16)*298;cb=cb*292/256;cv=cv*292/256;}else yy<<=8;
   const int r=(yy+359*cv+128)>>8,g=(yy-88*cb-183*cv+128)>>8,b=(yy+454*cb+128)>>8;
   o[0]=(uint8_t)(r<0?0:r>255?255:r);o[1]=(uint8_t)(g<0?0:g>255?255:g);o[2]=(uint8_t)(b<0?0:b>255?255:b);o[3]=255;o+=4;
  }
 }
 [t replaceRegion:MTLRegionMake2D(0,0,ow,oh) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:ow*4];
 return true;
}

// 0257b: at bind: a converted video frame is converted again from its surface, as long as the surface lives.
static void sweetYCbCrRefresh(id<MTLTexture> t) {
 id src=objc_getAssociatedObject(t,"ycbcr-src");
 if(!src)return;
 IOSurfaceRef surface=(__bridge IOSurfaceRef)src;
 const uint32_t seed=IOSurfaceGetSeed(surface);
 NSNumber *last=objc_getAssociatedObject(t,"ycbcr-seed");
 static const bool always=access("/var/local/mtl2-videocache",F_OK)!=0;   // (the file: only when the seed moves)
 if(!always && last && [last unsignedIntValue]==seed)return;
 if(sweetYCbCrFill(t,surface,t.width,t.height))objc_setAssociatedObject(t,"ycbcr-seed",@(seed),OBJC_ASSOCIATION_RETAIN);
}

static id<MTLTexture> sweetYCbCrTexture(id device,MTLTextureDescriptor *d,IOSurfaceRef surface,void *base,size_t allocation) {
 const size_t w=IOSurfaceGetWidthOfPlane(surface,0),h=IOSurfaceGetHeightOfPlane(surface,0);
 const size_t cw=IOSurfaceGetWidthOfPlane(surface,1),ch=IOSurfaceGetHeightOfPlane(surface,1);
 if(!w || !h || IOSurfaceGetBytesPerElementOfPlane(surface,0)!=1 || IOSurfaceGetBytesPerElementOfPlane(surface,1)!=2 ||
    cw<(w+1)/2 || ch<(h+1)/2 || d.width>w || d.height>h || !d.width || !d.height)return nil;
 size_t yrow=IOSurfaceGetBytesPerRowOfPlane(surface,0),crow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
 if(!yrow)yrow=(w+63)&~(size_t)63;
 if(!crow)crow=(cw*2+63)&~(size_t)63;
 const uint8_t *y=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0),*c=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
 const uintptr_t lo=(uintptr_t)base,hi=lo+allocation;
 if(!y || !c || (uintptr_t)y<lo || (uintptr_t)c<lo || (uintptr_t)y+yrow*(h-1)+w>hi || (uintptr_t)c+crow*(ch-1)+cw*2>hi)return nil;

 static std::mutex lock;static NSMutableDictionary<NSNumber *,NSArray *> *cache;
 const uint32_t seed=IOSurfaceGetSeed(surface);
 NSNumber *key=@(((uint64_t)IOSurfaceGetID(surface)<<32)|((uint64_t)d.width<<16)|d.height);
 // 0257: video frames ('420v'/'x420': the software decoder's pool, Safari mp4 2026-11-11) are converted on every use, not
 // cached by seed: playback flickered between two frames, as if a pooled surface's new picture kept its old seed. Photos
 // ('420f') keep the cache. /var/local/mtl2-videocache restores caching for video too (to compare).
 static const bool videoCache=access("/var/local/mtl2-videocache",F_OK)==0;
 const uint32_t pf0=IOSurfaceGetPixelFormat(surface);
 const bool cacheable=videoCache || !(pf0=='420v' || pf0=='x420');
 if(cacheable){std::lock_guard<std::mutex> guard(lock);
  if(!cache)cache=[NSMutableDictionary dictionary];
  NSArray *hit=cache[key];
  if(hit && [hit[0] unsignedIntValue]==seed)return hit[1];}

 const size_t ow=d.width,oh=d.height;
 // 0224: converted to RGB here. 0217 stored Y/Cb/Cr raw in r/g/b on the assumption that QuartzCore's shaders convert;
 // they do not - the hardware YCbCr formats sample as RGB - so photos showed in false colours. '420v' is video range,
 // '420f' (and the rest) full range; BT.601 matrix (JPEG/HEIC photos' usual one).
 const uint32_t pf=IOSurfaceGetPixelFormat(surface);
 const bool video=pf=='420v' || pf=='x420';
 std::vector<uint8_t> rgba(ow*oh*4);
 for(size_t j=0;j<oh;j++){
  const uint8_t *yr=y+yrow*j,*cr=c+crow*(j/2);uint8_t *o=rgba.data()+ow*4*j;
  for(size_t i=0;i<ow;i++){
   int yy=yr[i],cb=cr[(i/2)*2]-128,cv=cr[(i/2)*2+1]-128;
   if(video){yy=(yy-16)*298;cb=cb*292/256;cv=cv*292/256;}else yy<<=8;   /* video range: Y 16-235, Cb/Cr 16-240 */
   const int r=(yy+359*cv+128)>>8,g=(yy-88*cb-183*cv+128)>>8,b=(yy+454*cb+128)>>8;   /* BT.601: 1.402, .344/.714, 1.772 */
   o[0]=(uint8_t)(r<0?0:r>255?255:r);o[1]=(uint8_t)(g<0?0:g>255?255:g);o[2]=(uint8_t)(b<0?0:b>255?255:b);o[3]=255;o+=4;
  }
 }
 /* 501 is the _sRGB variant: the sampler then linearizes, as the real format would */
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
  d.pixelFormat==501?MTLPixelFormatRGBA8Unorm_sRGB:MTLPixelFormatRGBA8Unorm width:ow height:oh mipmapped:NO];
 td.usage=MTLTextureUsageShaderRead;td.storageMode=MTLStorageModeShared;
 id<MTLTexture> t=[device newTextureWithDescriptor:td];
 if(!t)return nil;
 [t replaceRegion:MTLRegionMake2D(0,0,ow,oh) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:ow*4];
 // 0218: what the planes hold as seen from this process (all Y 0 and Cb/Cr 0 = memory that never got the picture here,
 // as the "seen here" probes found for other processes' BGRA surfaces; Y 16 / Cb Cr 128 = a black picture)
 uint64_t ysum=0,cbsum=0,crsum=0;unsigned ymin=255,ymax=0;size_t n=0;
 for(size_t j=0;j<oh;j+=16)for(size_t i=0;i<ow;i+=16){const unsigned yv=y[yrow*j+i];const uint8_t *cc=c+crow*(j/2)+(i/2)*2;
  ysum+=yv;cbsum+=cc[0];crsum+=cc[1];ymin=MIN(ymin,yv);ymax=MAX(ymax,yv);n++;}
 static std::atomic<uint32_t> told{0};
 if(told.fetch_add(1)<12)NBLOG(@"native-metal: surface %#x '%c%c%c%c' %zux%zu YCbCr 4:2:0 as RGBA8 %zux%zu (rows %zu/%zu, seed %u): "
  "Y mean %llu min %u max %u, Cb %llu, Cr %llu (%zu samples)",
  IOSurfaceGetID(surface),(char)(IOSurfaceGetPixelFormat(surface)>>24),(char)(IOSurfaceGetPixelFormat(surface)>>16),
  (char)(IOSurfaceGetPixelFormat(surface)>>8),(char)IOSurfaceGetPixelFormat(surface),w,h,ow,oh,yrow,crow,seed,
  n?ysum/n:0,ymin,ymax,n?cbsum/n:0,n?crsum/n:0,n);
 objc_setAssociatedObject(t,"ycbcr",[NSString stringWithFormat:@"ycbcr surf %#x %zux%zu as RGBA8",IOSurfaceGetID(surface),ow,oh],
  OBJC_ASSOCIATION_RETAIN);   // 0219
 if(!cacheable){   // 0257b: a video frame: converted again at every bind (sweetYCbCrRefresh)
  objc_setAssociatedObject(t,"ycbcr-src",(__bridge id)surface,OBJC_ASSOCIATION_RETAIN);
  objc_setAssociatedObject(t,"ycbcr-seed",@(seed),OBJC_ASSOCIATION_RETAIN);
 }
 if(!cacheable)return t;   // 0257
 std::lock_guard<std::mutex> guard(lock);
 if(cache.count>=24)[cache removeAllObjects];
 cache[key]=@[@(seed),t];
 return t;
}
@implementation NBDeviceMethods
- (NSString *)name {return gName;}
- (uint64_t)registryID {return gRegistryID;}
- (NSUInteger)maxBufferLength {return nmb_device_max_buffer_length(gDevice);}
// Metal's measured heap placement contract: the requested byte count, aligned to 256 bytes.
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 (void)options;
 MTLSizeAndAlign result = {length, 256};
 return result;
}
// 0207: a texture's room in a heap (MPS sizes its temporary-image heaps with this: mpsprobe's Gaussian blur stopped at
// "-[NativeBridgeMetalDevice heapTextureSizeAndAlignWithDescriptor:]: unrecognized selector"). The runtime has no query
// for a texture's real Vulkan size, so this errs high: each mip level's width and height rounded up to 64 texels (the
// GPU's tiling), rows to 256 bytes, a quarter more for compression metadata, every level page-aligned; 4 KiB alignment.
// Too high only wastes heap space; too low would make the heap refuse the texture.
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
 MTLSizeAndAlign result = {0, 4096};
 if(!d)return result;
 const NSUInteger type=(NSUInteger)d.textureType;
 const NSUInteger slices=MAX(d.arrayLength,(NSUInteger)1)*MAX(d.sampleCount,(NSUInteger)1)*(type==5||type==6?6:1);
 const NSUInteger bpp=sweetBytesPerTexel((NSUInteger)d.pixelFormat);
 NSUInteger total=0;
 for(NSUInteger l=0;l<MAX(d.mipmapLevelCount,(NSUInteger)1);l++){
  const NSUInteger w=(MAX(d.width>>l,(NSUInteger)1)+63)&~(NSUInteger)63,h=(MAX(d.height>>l,(NSUInteger)1)+63)&~(NSUInteger)63;
  const NSUInteger row=(w*bpp+255)&~(NSUInteger)255;
  NSUInteger level=row*h*MAX(d.depth>>l,(NSUInteger)1)*slices;
  level+=level/4;
  total+=(level+4095)&~(NSUInteger)4095;
 }
 result.size=total;
 return result;
}
- (BOOL)hasUnifiedMemory {return YES;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set;return NO;}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. They are empty here, so a limit or feature the runtime measured is answered
// by this file's own answers (installAnswers) and any other stays the base class's neutral 0/NO. Nothing is claimed that nothing measured.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
- (NSString *)productName {return gName;}
- (NSString *)vendorName {char name[64]={};nmb_device_vendor_name(gDevice,name,sizeof name);return [NSString stringWithUTF8String:name];}
- (MTLSize)maxThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@",lastError());
 return MTLSizeMake(s[0],s[1],s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing("NativeBridgeMetalDevice",sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing device %s",sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent {
 NmbEvent *handle=nmb_event_new(gDevice,false);if(!handle)return nil;
 NBEvent *event=[NBEvent new];event.handle=handle;return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent {
 NmbEvent *handle=nmb_event_new(gDevice,true);if(!handle)return nil;
 NBSharedEvent *event=[NBSharedEvent new];event.handle=handle;return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence {
 NmbFence *handle=nmb_fence_new(gDevice);if(!handle)return nil;
 NBFence *fence=[NBFence new];fence.handle=handle;return (id<MTLFence>)fence;
}
- (id<MTLCommandQueue>)newCommandQueue {
 NmbQueue *handle=nmb_queue_new(gDevice);if(!handle)return nil;
 NBQueue *queue=[NBQueue new];queue.handle=handle;return (id<MTLCommandQueue>)queue;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
 if(!d)return nil;
 NmbHeap *handle=nmb_heap_new(gDevice,d.type,d.size,(uint64_t)d.resourceOptions);
 if(!handle){NBLOG(@"native-metal: heap refused: %@",lastError());return nil;}
 NBHeap *heap=[NBHeap new];heap.handle=handle;heap.options=d.resourceOptions;heap.heapType=d.type;return (id<MTLHeap>)heap;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {
 NBResourceGroup *group=[NBResourceGroup new];
 NSMutableArray *held=[NSMutableArray arrayWithCapacity:count];
 for(NSUInteger i=0;i<count;i++)if(resources[i])[held addObject:resources[i]];
 group.resources=held;return group;
}
// SweetMetal2: a texture over raw memory, and the layout object for such memory (MTLTextureLayout SPI), in one linear layout of
// our choosing that both answer alike: rows of the runtime's required linear pitch, level 0 of a 2D texture, page-aligned
// memory (it is imported, not copied). Clients ask the layout for size and alignment, allocate, then wrap the memory
// (RenderBox's drawables; without these: "RBDisplayList: no drawable" and blank widgets). A caller that never asked
// gets the pitch its length implies where the required one does not fit.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *,NSUInteger))deallocator {
 const size_t page=(size_t)getpagesize();
 if(!bytes || !d || !length || ((uintptr_t)bytes&(page-1)) || d.textureType!=MTLTextureType2D || d.mipmapLevelCount>1 || d.arrayLength>1 || d.sampleCount>1){
  NBLOG(@"native-metal: newTextureWithBytesNoCopy refused (%p %lu type %lu levels %lu)",bytes,(unsigned long)length,(unsigned long)d.textureType,(unsigned long)d.mipmapLevelCount);
  return nil;
 }
 uint32_t bw=0,bh=0,bb=0;
 if(sweetBlock(d.pixelFormat,&bw,&bh,&bb)) {
  /* compressed: no linear texture of it exists, so a texture of its own, filled from the caller's blocks (packed rows) */
  const NSUInteger row=(d.width+bw-1)/bw*bb,rows=(d.height+bh-1)/bh;
  if(row*rows>length){NBLOG(@"native-metal: newTextureWithBytesNoCopy: %lux%lu fmt %lu needs %lu bytes, has %lu",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)(row*rows),(unsigned long)length);return nil;}
  MTLTextureDescriptor *own=[d copy];
  own.storageMode=MTLStorageModeShared;
  id<MTLTexture> texture=[(id<MTLDevice>)self newTextureWithDescriptor:own];
  if(!texture)return nil;
  [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
  SweetMemoryOwner *owner=[SweetMemoryOwner new];owner.deallocator=deallocator;owner.bytes=bytes;owner.length=length;
  objc_setAssociatedObject(texture,&sweetMemoryOwnerKey,owner,OBJC_ASSOCIATION_RETAIN);
  return texture;
 }
 uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(row && row*d.height>length && length%d.height==0)row=length/d.height;
 if(!row || row*d.height>length){NBLOG(@"native-metal: newTextureWithBytesNoCopy: %lux%lu fmt %lu does not fit %lu bytes",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)length);return nil;}
 id<MTLBuffer> buffer=[(id<MTLDevice>)self newBufferWithBytesNoCopy:bytes length:(length+page-1)&~(page-1) options:MTLResourceStorageModeShared deallocator:deallocator];
 if(!buffer)return nil;
 return (id<MTLTexture>)textureOnBuffer((NBBuffer *)buffer,d,0,row,false);
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed {
 (void)backed;
 if(!d || d.textureType!=MTLTextureType2D || d.mipmapLevelCount>1 || d.arrayLength>1 || d.sampleCount>1){
  NBLOG(@"native-metal: newTextureLayoutWithDescriptor refused (type %lu levels %lu)",(unsigned long)d.textureType,(unsigned long)d.mipmapLevelCount);
  return nil;
 }
 uint32_t bw=0,bh=0,bb=0;
 if(sweetBlock(d.pixelFormat,&bw,&bh,&bb))
  return [[SweetTextureLayout alloc] initWithBlocks:bw by:bh bytes:bb width:d.width height:d.height];
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row){NBLOG(@"native-metal: newTextureLayoutWithDescriptor: no linear pitch for fmt %lu: %@",(unsigned long)d.pixelFormat,lastError());return nil;}
 return [[SweetTextureLayout alloc] initWithRow:row height:d.height texel:sweetTexelBytes(d.pixelFormat)];
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {(void)count;return [(id<MTLDevice>)self newCommandQueue];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,nullptr);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {
 NmbBuffer *handle=nmb_buffer_new_no_copy(gDevice,pointer,length,options);
 if(!handle){NBLOG(@"native-metal: no-copy buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;
 buffer.wrapped=pointer;buffer.deallocator=deallocator;
 return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new(gDevice,&descriptor);noteFormat((uint32_t)descriptor.format,handle!=nullptr);
 if(!handle){NBLOG(@"native-metal: texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;return (id<MTLTexture>)texture;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 // guacamole: planar surfaces too (QuartzCore's 'b3a8' tiles, one plane after xnu 0143): the texture is the plane's texels,
 // at its offset in the surface's import, with its row stride
 const size_t planes=surface?IOSurfaceGetPlaneCount(surface):0;
 // 0209: say which: SBRendererService logged "plane 0 of 0", which plane 0 of an unplanar surface is not (that passes),
 // so its surface or descriptor was nil (the wallpaper went black after a photo was chosen).
 if(!surface || !d){NBLOG(@"native-metal: surface texture refused: %s%s",surface?"":"no IOSurface ",d?"":"no descriptor");
  // 0213: who passes no surface (PosterBoard and SBRendererService, with a photo wallpaper set): the caller's frames,
  // the first two times in a process.
  static std::atomic<uint32_t> traced{0};
  if(traced.fetch_add(1)<2){
   NSArray<NSString *> *frames=[NSThread callStackSymbols];
   NSMutableString *text=[NSMutableString string];
   for(NSUInteger i=1;i<frames.count && i<18;i++)[text appendFormat:@"\n  %@",frames[i]];
   NBLOG(@"native-metal: no-IOSurface caller (%lux%lu format %lu):%@",(unsigned long)d.width,(unsigned long)d.height,
    (unsigned long)d.pixelFormat,text);
  }
  return nil;}
 if(planes?plane>=planes:plane!=0){NBLOG(@"native-metal: surface texture refused: plane %lu of %lu",(unsigned long)plane,(unsigned long)planes);return nil;}
 void *base=IOSurfaceGetBaseAddress(surface);const size_t page=(size_t)getpagesize(),allocation=IOSurfaceGetAllocSize(surface);
 if(!page || (page&(page-1)) || !base || ((uintptr_t)base&(page-1)) || !allocation || allocation>SIZE_MAX-(page-1)){
  // 0180: this refusal was silent; a backboardd composition of another process's surface (a game's drawable) then
  // drew nothing (suspected: the black compositions). Logged once per surface ID.
  static std::mutex lock;static NSMutableSet<NSNumber *> *said;
  std::lock_guard<std::mutex> guard(lock);if(!said)said=[NSMutableSet set];
  NSNumber *sid=@(IOSurfaceGetID(surface));
  if(![said containsObject:sid] && said.count<200){[said addObject:sid];
   const uint32_t f=IOSurfaceGetPixelFormat(surface);
   NBLOG(@"native-metal: surface %#x '%c%c%c%c' %zux%zu texture refused: base %p alloc %zu (no mapping in this process?)",
    IOSurfaceGetID(surface),(char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),base,allocation);}
  return nil;
 }
 const size_t bytes=(allocation+page-1)&~(page-1);
 const uintptr_t at=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
 if(!row)row=IOSurfaceGetBytesPerRow(surface);
 // 0216: QuartzCore's '_RB4' surfaces (RGB8_444_2P, 542) are read here as one BGRA8 plane (sweetNarrow) at the
 // surface's row. Until kernel 0215 their plane 0 reported row 0, so that row was used; now plane 0 reports its own
 // 1-byte row (465 wide: 512) and every import was refused ("RowPitch bytes_per_row 512, row_bytes 1860", backboardd).
 // Keep the surface's row for them, as before 0215.
 if(d && d.pixelFormat==542 && IOSurfaceGetBytesPerRow(surface)>row)row=IOSurfaceGetBytesPerRow(surface);
 if(!row && sweetNarrow(d.pixelFormat)!=(uint64_t)d.pixelFormat)row=IOSurfaceGetWidth(surface)*4;  // 10-bit asked over BGRA
 if(at<(uintptr_t)base || at-(uintptr_t)base>=allocation){NBLOG(@"native-metal: surface texture refused: plane %lu outside the surface",(unsigned long)plane);return nil;}
 // 0217: two-plane YCbCr 4:2:0 ('420f'/'420v') sampled as one texture (MTLPixelFormatYCBCR8_420_2P(_sRGB), 500/501).
 // backboardd composites a photo wallpaper (PhotosPosterProvider's '420f' image) this way and the runtime has no such
 // format ("Unsupported ... MTLPixelFormatYCBCR8_420_2P ... NotImplemented", thousands a minute): the lock and home
 // screens stayed black while the picker's BGRA snapshot showed the photo. Here the planes are expanded on the CPU into
 // an RGBA8 texture holding what the hardware format samples as: RGB (0224: converted here, BT.601, the surface's
 // range; 0217 stored raw Y/Cb/Cr, which showed photos in false colours). Kept per surface and rebuilt only when its seed changes.
 if((d.pixelFormat==500 || d.pixelFormat==501) && planes==2 && plane==0)
  if(id<MTLTexture> t=sweetYCbCrTexture(self,d,surface,base,allocation))return t;
 NBBuffer *buffer=surfaceBuffer(surface,base,bytes);if(!buffer)return nil;
 bool hasYCbCrPlanes=planes>=2;
 uint64_t chromaOffset=0,chromaRow=0;
 if(hasYCbCrPlanes){
  void *chroma=IOSurfaceGetBaseAddressOfPlane(surface,1);
  if(!chroma || (uintptr_t)chroma<(uintptr_t)base || (uintptr_t)chroma-(uintptr_t)base>=allocation)hasYCbCrPlanes=false;
  else {chromaOffset=(uintptr_t)chroma-(uintptr_t)base;chromaRow=IOSurfaceGetBytesPerRowOfPlane(surface,1);}
 }
 NBTexture *texture=textureOnBuffer(buffer,d,at-(uintptr_t)base,row,true,hasYCbCrPlanes,plane,IOSurfaceGetPixelFormat(surface),
                                    IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chromaOffset,chromaRow);
 if(!texture && d.width==IOSurfaceGetHeight(surface) && d.height==IOSurfaceGetWidth(surface) && d.width!=d.height){
  // 0189: a landscape view of a portrait surface (CoreAnimation's framebuffer while a landscape app is in front). Apple's
  // driver takes such a texture with a private rotation on the descriptor; log what the descriptor carries, once each.
  static std::atomic<uint32_t> said{0};
  if(said.fetch_add(1)<6){
   NSMutableString *spi=[NSMutableString string];
   for(NSString *key in @[@"rotation",@"swizzle",@"compressionType",@"framebufferOnly",@"forceResourceIndex",@"textureLayout"]){
    SEL sel=NSSelectorFromString(key);
    if([d respondsToSelector:sel]){
     id value=nil;@try{value=[d valueForKey:key];}@catch(NSException *e){value=@"?";}
     [spi appendFormat:@" %@=%@",key,value];
    }
   }
   NBLOG(@"native-metal: rotated view asked: surface %#x %zux%zu as %lux%lu usage %lu storage %lu type %lu;%@",IOSurfaceGetID(surface),
    IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.usage,
    (unsigned long)d.storageMode,(unsigned long)d.textureType,spi.length?spi:@" (no rotation property on the descriptor)");
  }
 }
 if(!texture && !planes && d.width==IOSurfaceGetHeight(surface) && d.height==IOSurfaceGetWidth(surface) && d.width!=d.height){
  if(id<MTLTexture> rotated=rotatedSurfaceTexture(self,d,surface))return rotated;   // 0196
 }
 // 0212: a YCbCr photo's chroma plane did not fit its own surface (PhotosPosterProvider, photo wallpaper: '420f'
 // 606x1145, plane 1 as RG8 303x573 with the plane's row 968: "PastBuffer counted 1291646 length 1114112"). The
 // allocation fits rows of the plane's own width (303 x 2 bytes, 64-aligned: 640), so the surface's reported row is
 // suspect. Log every plane's geometry (once per surface), and if a 64-aligned row of the plane's width fits, use it.
 if(!texture && planes>=2 && plane<planes){
  static std::mutex lock;static NSMutableSet<NSNumber *> *said;
  {std::lock_guard<std::mutex> guard(lock);if(!said)said=[NSMutableSet set];
   NSNumber *sid=@(IOSurfaceGetID(surface));
   if(![said containsObject:sid] && said.count<20){[said addObject:sid];
    NSMutableString *geo=[NSMutableString string];
    for(size_t i=0;i<planes;i++)
     [geo appendFormat:@" [%zu: offset %ld %zux%zu row %zu elem %zu bytes]",i,
      (long)((uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,i)-(uintptr_t)base),IOSurfaceGetWidthOfPlane(surface,i),
      IOSurfaceGetHeightOfPlane(surface,i),IOSurfaceGetBytesPerRowOfPlane(surface,i),IOSurfaceGetBytesPerElementOfPlane(surface,i)];
    NBLOG(@"native-metal: surface %#x planes: alloc %zu, row %zu;%@",IOSurfaceGetID(surface),allocation,
     IOSurfaceGetBytesPerRow(surface),geo);
   }}
  const size_t pw=IOSurfaceGetWidthOfPlane(surface,plane),ph=IOSurfaceGetHeightOfPlane(surface,plane),
               pe=IOSurfaceGetBytesPerElementOfPlane(surface,plane),tight=(pw*pe+63)&~(size_t)63;
  if(pw && ph && pe && tight<row && (at-(uintptr_t)base)+tight*(ph-1)+pw*pe<=allocation){
   texture=textureOnBuffer(buffer,d,at-(uintptr_t)base,tight,true,hasYCbCrPlanes,plane,IOSurfaceGetPixelFormat(surface),
                           IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chromaOffset,plane==1?tight:chromaRow);
   static std::atomic<uint32_t> told{0};
   if(told.fetch_add(1)<6)NBLOG(@"native-metal: surface %#x plane %lu: row %zu does not fit, row %zu %s",IOSurfaceGetID(surface),
    (unsigned long)plane,row,tight,texture?"used":"refused too");
   if(texture)row=tight;
  }
 }
 if(!texture){const uint32_t f=IOSurfaceGetPixelFormat(surface);
  NBLOG(@"native-metal: surface %#x '%c%c%c%c' %zux%zu plane %lu: format %lu %lux%lu, row %zu refused",IOSurfaceGetID(surface),
   (char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),
   (unsigned long)plane,(unsigned long)d.pixelFormat,(unsigned long)d.width,(unsigned long)d.height,row);return nil;}
 texture.surface=(IOSurfaceRef)CFRetain(surface);
 // 0180: is another process's full-screen picture (a game's drawable) visible here? Once per surface ID: 64 samples
 // through this process's own mapping (caches invalidated first), black = RGB sum <= 24. In backboardd: content here
 // but black compositions -> the GPU side; black here while the game's own pre-present samples have content -> the
 // mapping in this process is not the game's memory.
 if(IOSurfaceGetWidth(surface)*IOSurfaceGetHeight(surface)>=2000000 /* 0187: landscape too */ && !planes &&
    (IOSurfaceGetPixelFormat(surface)=='BGRA' || IOSurfaceGetPixelFormat(surface)=='RGBA')){
  static std::mutex lock;static NSMutableSet<NSNumber *> *seen;
  std::lock_guard<std::mutex> guard(lock);if(!seen)seen=[NSMutableSet set];
  NSNumber *sid=@(IOSurfaceGetID(surface));
  if(![seen containsObject:sid] && seen.count<64){[seen addObject:sid];
   const size_t w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface);unsigned black=0;
   for(unsigned j=0;j<8;j++){
    uint8_t *rowp=(uint8_t *)base+row*(h*(2*j+1)/16);
    sys_dcache_flush(rowp,row);
    for(unsigned i=0;i<8;i++){const uint8_t *px=rowp+4*(w*(2*i+1)/16);if(px[0]+px[1]+px[2]<=24)black++;}
   }
   // 0181: the same 64 points as this process's GPU reads them through the new texture (blit to a buffer)
   unsigned gpuBlack=65;
   @autoreleasepool {
    static id<MTLCommandQueue> probeQueue;static dispatch_once_t once;
    dispatch_once(&once,^{probeQueue=[(id<MTLDevice>)gDeviceObject newCommandQueue];});
    id<MTLBuffer> out=[(id<MTLDevice>)gDeviceObject newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[probeQueue commandBuffer];
    id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
    if(out && be){
     memset(out.contents,0x7f,256);
     for(unsigned j=0;j<8;j++)for(unsigned i=0;i<8;i++)
      [be copyFromTexture:(id<MTLTexture>)texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(w*(2*i+1)/16,h*(2*j+1)/16,0)
               sourceSize:MTLSizeMake(1,1,1) toBuffer:out destinationOffset:4*(j*8+i) destinationBytesPerRow:4 destinationBytesPerImage:4];
     [be endEncoding];[cb commit];[cb waitUntilCompleted];
     if(cb.status==MTLCommandBufferStatusCompleted){
      const uint8_t *px=(const uint8_t *)out.contents;gpuBlack=0;
      for(unsigned k=0;k<64;k++)if(px[4*k]+px[4*k+1]+px[4*k+2]<=24)gpuBlack++;
     }
    }
   }
   if(black<32){std::lock_guard<std::mutex> g2(gWatchLock);if(!gWatchedSurfaces)gWatchedSurfaces=[NSMutableSet set];[gWatchedSurfaces addObject:@(IOSurfaceGetID(surface))];}
   NBLOG(@"native-metal: surface %#x %zux%zu seen here: %u/64 black by CPU, %u/64 by GPU (65 = probe failed) (base %p, alloc %zu, label %@)",
    IOSurfaceGetID(surface),w,h,black,gpuBlack,base,allocation,
    d.pixelFormat==MTLPixelFormatBGRA8Unorm?@"BGRA8":[NSString stringWithFormat:@"fmt %lu",(unsigned long)d.pixelFormat]);
  }
 }
 return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes,size,false);(void)mapped;gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
 // Ask Apple's inherited device implementation to compile MSL with the process's real iOS compiler service. Its
 // result is a native _MTLLibrary, so extract the compiled metallib and reopen it through our bridge implementation.
 SEL selector=@selector(newLibraryWithSource:options:error:);
 struct objc_super superInfo=nativeDeviceSuper(self);
 id<MTLLibrary> (*compile)(struct objc_super *,SEL,NSString *,MTLCompileOptions *,NSError **)=
  (id<MTLLibrary> (*)(struct objc_super *,SEL,NSString *,MTLCompileOptions *,NSError **))objc_msgSendSuper;
 NSError *compileError=nil;
 id<MTLLibrary> nativeLibrary=compile(&superInfo,selector,source,options,&compileError);
 if(!nativeLibrary){if(error)*error=compileError;return nil;}
 const uint64_t began=nowUs();
 id<MTLLibrary> library=bridgeCompiledLibrary(self,nativeLibrary,error);
 gTimeLibrary.add(began);
 return library;
}
- (void)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
 if(!completionHandler)return;
 SEL selector=@selector(newLibraryWithSource:options:completionHandler:);
 struct objc_super superInfo=nativeDeviceSuper(self);
 void (*compile)(struct objc_super *,SEL,NSString *,MTLCompileOptions *,MTLNewLibraryCompletionHandler)=
  (void (*)(struct objc_super *,SEL,NSString *,MTLCompileOptions *,MTLNewLibraryCompletionHandler))objc_msgSendSuper;
 compile(&superInfo,selector,source,options,^(id<MTLLibrary> nativeLibrary,NSError *compileError){
  if(!nativeLibrary){completionHandler(nil,compileError);return;}
  NSError *bridgeError=nil;
  const uint64_t began=nowUs();
  id<MTLLibrary> library=bridgeCompiledLibrary(self,nativeLibrary,&bridgeError);
  gTimeLibrary.add(began);
  completionHandler(library,bridgeError);
 });
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error]:nil;if(!bytes)return nil;
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes.bytes,bytes.length,false);gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=nbError(@"the bundle has no default.metallib");return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary {return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 if(![(id)function isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign function");return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,nullptr,0};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_options(gDevice,[(NBFunction *)function handle],&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                                                       reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return nil;
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error))return nil;
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_descriptor(gDevice,&p,&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;state.label=d.label;gPipelines++;
 if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 auto *p=new NmbRenderPipelineDescriptor();
 if(!renderDescriptor(d,p,error)){delete p;return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error)){delete p;return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();const NBCost costBegan=NBCost::now();
 NmbRenderPipeline *handle=nmb_render_pipeline_new_with_options(gDevice,p,&creation);delete p;gTimePipeline.add(pipelineBegan);
 journalRenderPipeline(d,pipelineBegan,handle?nullptr:nmb_last_error(),gJournalPath.empty()?std::string():NBCost::now().since(costBegan));
 if(!handle){NBLOG(@"native-metal: render pipeline refused (vertex %@, fragment %@): %@",d.vertexFunction.name,d.fragmentFunction.name,lastError());
  return failWith<id<MTLRenderPipelineState>>(error,@"render pipeline",nil);}
 NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;
 {// 0182: what the pipeline is, for the draw trace of full-screen surface textures
  MTLRenderPipelineColorAttachmentDescriptor *c=d.colorAttachments[0];
  objc_setAssociatedObject(state,"desc",[NSString stringWithFormat:@"label=%@ vf=%@ ff=%@ fmt=%lu blend=%d rgb=%lu:%lu:%lu a=%lu:%lu:%lu mask=%lu samples=%lu",
   d.label?:@"",d.vertexFunction.name?:@"-",d.fragmentFunction.name?:@"-",(unsigned long)c.pixelFormat,(int)c.blendingEnabled,
   (unsigned long)c.sourceRGBBlendFactor,(unsigned long)c.destinationRGBBlendFactor,(unsigned long)c.rgbBlendOperation,
   (unsigned long)c.sourceAlphaBlendFactor,(unsigned long)c.destinationAlphaBlendFactor,(unsigned long)c.alphaBlendOperation,
   (unsigned long)c.writeMask,(unsigned long)d.rasterSampleCount],OBJC_ASSOCIATION_RETAIN);
 }
 return (id<MTLRenderPipelineState>)state;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error {
 if(d.url && !d.url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 NmbBinaryArchive *handle=nmb_binary_archive_new(gDevice,d.url?d.url.fileSystemRepresentation:nullptr);
 if(!handle)return failWith<id<MTLBinaryArchive>>(error,@"binary archive",nil);
 NBBinaryArchive *archive=[NBBinaryArchive new];archive.handle=handle;if(error)*error=nil;return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d {
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row)NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@",lastError());
 return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
 NmbSamplerDescriptor s={(uint64_t)d.minFilter,(uint64_t)d.magFilter,(uint64_t)d.mipFilter,(uint64_t)d.sAddressMode,(uint64_t)d.tAddressMode,(uint64_t)d.rAddressMode,
     (uint64_t)d.borderColor,(uint64_t)d.normalizedCoordinates,d.maxAnisotropy,(uint64_t)d.compareFunction,(uint64_t)d.lodAverage,d.lodMinClamp,d.lodMaxClamp};
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: sampler refused: %@",lastError());return nil;}
 NBSampler *sampler=[NBSampler new];sampler.handle=handle;
 objc_setAssociatedObject(sampler,"desc",[NSString stringWithFormat:@"min=%lu mag=%lu mip=%lu addr=%lu,%lu norm=%d border=%lu",
  (unsigned long)d.minFilter,(unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,
  (int)d.normalizedCoordinates,(unsigned long)d.borderColor],OBJC_ASSOCIATION_RETAIN);
 return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 auto face=[](MTLStencilDescriptor *s)->NmbStencil {
  return {(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,(uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 NmbDepthStencilDescriptor s={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 NmbDepthStencil *handle=nmb_depth_stencil_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: depth/stencil state refused: %@",lastError());return nil;}
 NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime each call, and a refusal answers 0/NO
// with the reason logged.
static std::unordered_map<SEL,uint64_t> gAnswers;
static NSUInteger countAnswer(id,SEL cmd) {return (NSUInteger)gAnswers.at(cmd);}
static BOOL flagAnswer(id,SEL cmd) {return gAnswers.at(cmd)!=0;}
// guacamole: answers with an argument are the device's too and never change; asking the runtime each call cost 29% of backboardd's
// render thread (supportsFamily: per draw, each a full family audit cloning the format tables: tsample, mtl2 run 11). Remembered per
// (selector, argument), refusals included (logged once).
struct NBAnswerKey {SEL sel;NSUInteger arg;bool operator==(const NBAnswerKey &o) const {return sel==o.sel && arg==o.arg;}};
struct NBAnswerHash {size_t operator()(const NBAnswerKey &k) const {return std::hash<void *>()((void *)k.sel)^(std::hash<NSUInteger>()(k.arg)*31);}};
static std::unordered_map<NBAnswerKey,uint64_t,NBAnswerHash> gArgAnswers;
static os_unfair_lock gArgAnswersLock=OS_UNFAIR_LOCK_INIT;
static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 const NBAnswerKey key={cmd,arg};
 os_unfair_lock_lock(&gArgAnswersLock);
 auto it=gArgAnswers.find(key);
 if(it!=gArgAnswers.end()){uint64_t value=it->second;os_unfair_lock_unlock(&gArgAnswersLock);return value;}
 os_unfair_lock_unlock(&gArgAnswersLock);
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());value=0;}
 os_unfair_lock_lock(&gArgAnswersLock);
 gArgAnswers[key]=value;
 os_unfair_lock_unlock(&gArgAnswersLock);
 return value;
}
static NSUInteger countArgAnswer(id,SEL cmd,NSUInteger arg) {return (NSUInteger)askAnswer(cmd,arg);}
static BOOL flagArgAnswer(id,SEL cmd,NSUInteger arg) {return askAnswer(cmd,arg)!=0;}
static void installAnswers(Class cls) {
 uint32_t kind=0;
 for(size_t i=0;const char *name=nmb_device_answer_selector(i,&kind);i++) {
  SEL sel=sel_registerName(name);uint64_t value=0;
  switch(kind) {
  case NMB_ANSWER_COUNT: case NMB_ANSWER_FLAG:
   if(!nmb_device_answer(gDevice,name,0,&value)){NBLOG(@"native-metal: no device answer: %@",lastError());continue;}
   // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
   // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
   // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
   if(!strcmp(name,"supportsExtendedXR10Formats"))value=0;
   gAnswers[sel]=value;
   class_addMethod(cls,sel,kind==NMB_ANSWER_COUNT?(IMP)countAnswer:(IMP)flagAnswer,kind==NMB_ANSWER_COUNT?"Q16@0:8":"B16@0:8");
   break;
  case NMB_ANSWER_COUNT_ARG: class_addMethod(cls,sel,(IMP)countArgAnswer,"Q24@0:8Q16");break;
  case NMB_ANSWER_FLAG_ARG: class_addMethod(cls,sel,(IMP)flagArgAnswer,"B24@0:8Q16");break;
  }
 }
}

// 0203: Core Image's graph kernels. With CI_ENABLE_METAL_DAG / CI_ENABLE_FUNCTION_STITCHING on (iOS's default; read
// with getenv at first use, CoreImage CI::MetalDAGHelper::analyze_kernels), Core Image builds kernels with
// -[MTLDevice newLibraryWithStitchedDescriptor:...]: Apple's MTLLibraryBuilder then asks each function for
// -libraryData, which ours cannot give (PhotosPosterProvider, choosing a photo for a wallpaper: "-[NBFunction
// libraryData]: unrecognized selector"); stitched visible functions are not what metal2vulkan runs either. With both 0
// Core Image writes each filter chain as Metal source and compiles it (newLibraryWithSource, which works here).
// Set when this bundle loads (before Core Image reads them); a value already in the environment is kept.
__attribute__((constructor)) static void sweetCoreImageWithoutStitching(void) {
 setenv("CI_ENABLE_METAL_DAG","0",0);
 setenv("CI_ENABLE_FUNCTION_STITCHING","0",0);
}
// 0244: how an app hands its CAMetalLayer frames to the screen (Maps' map stays black, 0243): every nextDrawable
// (the layer's size, format, presentsWithTransaction, framebufferOnly; the first 5 then every 300th) and every
// -[drawable present] (direct or from presentDrawable:; same sampling). Not in backboardd.
static IMP gOrigNextDrawable,gOrigPresent;
// 0245: before a drawable goes to CoreAnimation by its own -present (presentsWithTransaction: Maps commits and calls
// [drawable present] without waitUntilScheduled, so 0243 did not run for most frames), wait for the GPU on every
// in-flight command buffer that uses the drawable's texture. GPU only (nmb_command_buffer_wait), never the
// completed handlers: present runs on the main thread, where those handlers may be queued.
static void waitGPUForTexture(id<MTLTexture> texture) {
 if(![(id)texture isKindOfClass:[NBTexture class]])return;
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending){
  if(commandBuffer.failedLocally || !commandBuffer.handle)continue;
  if(nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)texture handle])!=0){
   nmb_command_buffer_wait(commandBuffer.handle,0);
   static std::atomic<uint64_t> waits{0};const uint64_t w=++waits;
   if(w<=3 || w%300==0)NBLOG(@"native-metal: drawable present waited for its GPU work #%llu (0245)",(unsigned long long)w);
  }
 }
}
static void hookDrawables() {
 Class layerClass=NSClassFromString(@"CAMetalLayer");
 Method next=layerClass?class_getInstanceMethod(layerClass,@selector(nextDrawable)):nullptr;
 if(!next)return;
 gOrigNextDrawable=method_getImplementation(next);
 method_setImplementation(next,imp_implementationWithBlock(^id(CAMetalLayer *layer){
  id drawable=((id(*)(id,SEL))gOrigNextDrawable)(layer,@selector(nextDrawable));
  static std::atomic<uint64_t> asked{0};const uint64_t k=++asked;
  if(k<=5 || k%300==0)NBLOG(@"native-metal: nextDrawable #%llu layer %p '%@' %.0fx%.0f fmt %lu presentsWithTransaction=%d framebufferOnly=%d -> %@",
   (unsigned long long)k,layer,layer.name?:@"",layer.drawableSize.width,layer.drawableSize.height,(unsigned long)layer.pixelFormat,
   (int)layer.presentsWithTransaction,(int)layer.framebufferOnly,drawable?NSStringFromClass([drawable class]):@"nil");
  static dispatch_once_t once;
  if(drawable)dispatch_once(&once,^{
   Method present=class_getInstanceMethod([drawable class],@selector(present));
   if(!present)return;
   gOrigPresent=method_getImplementation(present);
   method_setImplementation(present,imp_implementationWithBlock(^(id d){
    static std::atomic<uint64_t> presented{0};const uint64_t j=++presented;
    if(j<=5 || j%300==0)NBLOG(@"native-metal: -[drawable present] #%llu",(unsigned long long)j);
    if([d respondsToSelector:@selector(texture)])waitGPUForTexture([(id<CAMetalDrawable>)d texture]);   // 0245
    // 0246: what the frame holds when it is handed over (8x8 samples; 65 = could not sample): first 10, then every 300th
    if((j<=10 || j%300==0) && [d respondsToSelector:@selector(texture)]){
     id<MTLTexture> t=[(id<CAMetalDrawable>)d texture];
     const unsigned black=prePresentBlackSamples(t);
     IOSurfaceRef sf=[(id)t isKindOfClass:[NBTexture class]]?[(NBTexture *)t surface]:nullptr;
     NBLOG(@"native-metal: present #%llu frame %lux%lu fmt %lu surf %#x: %u/64 black (0246)",(unsigned long long)j,(unsigned long)t.width,
      (unsigned long)t.height,(unsigned long)t.pixelFormat,sf?IOSurfaceGetID(sf):0,black);
    }
    ((void(*)(id,SEL))gOrigPresent)(d,@selector(present));
   }));
  });
  return drawable;
 }));
}
extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];
 gDevice=nmb_device_open(loader,cache);
 if(!gDevice){NBLOG(@"native-metal: device refused: %s",nmb_last_error());return nil;}
 if(strcmp(getprogname(),"backboardd")!=0)hookDrawables();   // 0244
 if(cache){gJournalPath=std::string(cache)+"/pipelines.log";gJournalBegan=nowUs();}
 char name[128]={};nmb_device_name(gDevice,name,sizeof name);gName=[NSString stringWithUTF8String:name];gRegistryID=registryID;
 // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
 {char waived[4096]={};nmb_device_family_waivers(gDevice,waived,sizeof waived);
  for(char *line=waived,*next;*line;line=next){next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);journal(std::string("F ")+line);}}
 Class base=objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
 // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
 // own _MTLDevice implementation.
 if(getenv("NMB_DEVICE_BASE_NSOBJECT"))base=[NSObject class];
#endif
#if TARGET_OS_IPHONE
 // The dynamic subclass adds no instance variables, so any base size works (iPadOS 17.7.11: 536 bytes).
 if(!base){NBLOG(@"native-metal: no _MTLDevice");return nil;}
 NBLOG(@"native-metal: _MTLDevice is %zu bytes",class_getInstanceSize(base));
#endif
 if(!base)base=[NSObject class];
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;
 unsigned count=0;Method *methods=class_copyMethodList([NBDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));
 // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
 // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
 if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 // SweetMetal2: Metal checks textures against its private implementation protocol too, and NSLogs "texture is not a
 // MTLTextureImplementation." on every miss (~600 lines/s from backboardd while scrolling).
 {Protocol *tp=objc_getProtocol("MTLTextureImplementation");if(tp)class_addProtocol([NBTexture class],tp);
  NBLOG(@"native-metal: MTLTextureImplementation %s",tp?([NBTexture conformsToProtocol:tp]?"adopted":"not adopted"):"missing");}
 // 0178: the same check for the rest. QuartzCore in backboardd logged "fragmentFunction is not a MTLFunction." /
 // "vertexFunction is not a MTLFunction." from boot on: Metal's pipeline descriptor checks conformance, and these classes
 // only answered the selectors. A refused function leaves CoreAnimation without that pipeline, and what it would have
 // drawn is skipped (suspected: the black compositions of a game's layer).
 {
  struct {Class cls;const char *names[3];} adopt[]={
   // Public protocols only, named by the message: a private one could make callers take SPI paths this file lacks.
   {[NBFunction class],{"MTLFunction",nullptr,nullptr}},
   {[NBLibrary class],{"MTLLibrary",nullptr,nullptr}},
   // 0206: Metal Performance Shaders checks the command buffer: -[MPSCommandBufferImageCache initWithCommandBuffer:]
   // returns nil unless [commandBuffer conformsToProtocol:@protocol(MTLCommandBuffer)], and MPSAutoCache then reads
   // nil+0x18 (PhotosPosterProvider choosing a photo wallpaper; PridePosterExtension). The rest for the same reason.
   {[NBCommandBuffer class],{"MTLCommandBuffer",nullptr,nullptr}},
   {[NBQueue class],{"MTLCommandQueue",nullptr,nullptr}},
   {[NBBuffer class],{"MTLBuffer","MTLResource",nullptr}},
   {[NBTexture class],{"MTLTexture","MTLResource",nullptr}},
   {[NBRenderEncoder class],{"MTLRenderCommandEncoder","MTLCommandEncoder",nullptr}},
   {[NBComputeEncoder class],{"MTLComputeCommandEncoder","MTLCommandEncoder",nullptr}},
   {[NBBlitEncoder class],{"MTLBlitCommandEncoder","MTLCommandEncoder",nullptr}},
  };
  for(auto &a:adopt)for(const char *name:a.names){
   if(!name)continue;
   Protocol *p=objc_getProtocol(name);
   // 0210: objc_getProtocol finds a protocol only once some loaded image has registered it, and Metal's own binaries
   // register some only where they use them: SBRendererService logged "NBRenderEncoder MTLRenderCommandEncoder
   // missing" and "NBBlitEncoder MTLBlitCommandEncoder missing" (PosterBoard, with more of Metal loaded, adopted both),
   // so its encoders failed conformance checks there. These are public protocols from the SDK headers: referencing
   // them with @protocol puts them in this bundle's own protocol list, so they always exist here.
   if(!p){
    static const struct {const char *name;Protocol *__unsafe_unretained proto;} publicProtocols[]={
     {"MTLFunction",@protocol(MTLFunction)},{"MTLLibrary",@protocol(MTLLibrary)},
     {"MTLCommandBuffer",@protocol(MTLCommandBuffer)},{"MTLCommandQueue",@protocol(MTLCommandQueue)},
     {"MTLBuffer",@protocol(MTLBuffer)},{"MTLResource",@protocol(MTLResource)},{"MTLTexture",@protocol(MTLTexture)},
     {"MTLRenderCommandEncoder",@protocol(MTLRenderCommandEncoder)},{"MTLComputeCommandEncoder",@protocol(MTLComputeCommandEncoder)},
     {"MTLBlitCommandEncoder",@protocol(MTLBlitCommandEncoder)},{"MTLCommandEncoder",@protocol(MTLCommandEncoder)},
    };
    for(const auto &pp:publicProtocols)if(!strcmp(pp.name,name))p=pp.proto;
   }
   if(p && !class_conformsToProtocol(a.cls,p))class_addProtocol(a.cls,p);
   NBLOG(@"native-metal: %s %s %s",class_getName(a.cls),name,p?([a.cls conformsToProtocol:p]?"adopted":"not adopted"):"missing");
  }
 }
 objc_registerClassPair(cls);
 gDeviceObject=[cls new];
 // 0200: the base class's limits stay 0 (initLimits is empty here, above), and some clients read them directly through the
 // private -[MTLDevice limits] (a pointer to _MTLDevice's own table). Photos (PhotosUICore -[PXGMetalTextureConverter
 // createTextureFromCGImage:transform:options:]) takes limits+0x84/+0x88 (uint32 maximum 2D texture width/height) as the most it
 // may draw an image at: 0 x 0 made every redraw 0 x 0 and it aborted on 'Invalid parameter not satisfying:
 // !PXPixelSizeAreaIsZero(roundedSize)' at its first view. Those two get 16384, the A11's and Turnip's (Adreno 640) 2D limit, when 0.
 {SEL limitsSel=sel_registerName("limits");
  if([gDeviceObject respondsToSelector:limitsSel]){
   uint8_t *limits=((uint8_t *(*)(id,SEL))objc_msgSend)(gDeviceObject,limitsSel);
   if(limits){
    uint32_t w=0,h=0;memcpy(&w,limits+0x84,4);memcpy(&h,limits+0x88,4);
    // only in the device object itself (_MTLDevice's own ivar table), never through a pointer elsewhere (read-only memory)
    const uintptr_t obj=(uintptr_t)(__bridge void *)gDeviceObject,end=obj+class_getInstanceSize(cls);
    const bool inside=(uintptr_t)limits>=obj && (uintptr_t)limits+0x8c<=end;
    if(!w && !h && inside){const uint32_t max2d=16384;memcpy(limits+0x84,&max2d,4);memcpy(limits+0x88,&max2d,4);}
    if(!inside)NBLOG(@"native-metal: limits %p is not inside the device (%p, %zu bytes): left alone",limits,(void *)obj,(size_t)(end-obj));
    memcpy(&w,limits+0x84,4);memcpy(&h,limits+0x88,4);
    NBLOG(@"native-metal: limits at %p: 2D texture %ux%u (+0x84/+0x88)",limits,w,h);
   }
  }}
 return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes,size_t length) {
 NmbLibrary *handle=gDevice?nmb_library_new(gDevice,bytes,length,true):nullptr;if(!handle)return nil;
 NBLibrary *library=[NBLibrary new];library.handle=handle;return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out) {
 return gDevice && nmb_device_shader_cache_stats(gDevice,out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10]) {
 return gDevice && nmb_device_work_stats(gDevice,out);
}

extern "C" size_t native_bridge_metal_status(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return (size_t)snprintf(out,size,"c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),(unsigned long long)gCompleted.load(),
  (unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),(unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),(unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load(),gLastProblems[0],gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out,size_t size) {
 int n=snprintf(out,size,"tex x%llu f",(unsigned long long)gTextureRefusals.load());
 for(unsigned i=0;i<gFormatCount && n>0 && (size_t)n<size;i++)n+=snprintf(out+n,size-(size_t)n,"%u ",gFormats[i]);
 return n>0?(size_t)n:0;
}
// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;
extern "C" size_t native_bridge_metal_benchmark(id device,const char *metallib,unsigned pairs,char *out,size_t size) {
 auto now=[]{return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);};
 // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
 const unsigned base=gBenchRuns++?2u:0u;
 auto put=[&](unsigned row,const char *format,auto... args){std::lock_guard<std::mutex> lock(gBenchLock);snprintf(gBench[base+row],sizeof gBench[base+row],format,args...);
  if(base)gBench[base+row][0]='Z';};
 // compute: a dependent integer chain (no memory), 100M steps
 uint64_t t=now(),x=0x9e3779b97f4a7c15ull;
 for(uint32_t i=0;i<100000000u;i++){x^=x<<13;x^=x>>7;x^=x<<17;}
 const uint64_t computeUs=(now()-t)/1000;
 // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
 const size_t bytes=32u<<20;
 uint64_t faultUs=0,fillUs=0,shaUs=0;
 void *m=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m!=MAP_FAILED) {
  volatile uint8_t *b=(volatile uint8_t *)m;
  t=now();for(size_t at=0;at<bytes;at+=4096)b[at]=1;faultUs=(now()-t)/1000;
  t=now();for(int k=0;k<4;k++)memset(m,k,bytes);fillUs=(now()-t)/1000;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  t=now();CC_SHA256(m,(CC_LONG)(16u<<20),digest);shaUs=(now()-t)/1000;
  x^=digest[0];
  munmap(m,bytes);
 }
 put(0,"Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",(unsigned long long)(computeUs/1000),
  (unsigned long long)faultUs,(unsigned long long)(faultUs*1000/8192),(unsigned long long)(fillUs?4ull*32*1000000/fillUs:0),
  (unsigned long long)(shaUs?16ull*1000000/shaUs:0),(unsigned long long)(x&0xf));
 // pipelines: the host tool's pairs, in its order
 uint64_t made=0,refused=0,createUs=0,createMax=0,drawUs=0,drawMax=0,libraryUs=0;
 @autoreleasepool {
  id<MTLDevice> dev=(id<MTLDevice>)device;NSError *error=nil;
  t=now();
  id<MTLLibrary> qc=metallib?[dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error]:nil;
  libraryUs=(now()-t)/1000;
  if(qc) {
   NSArray<NSString *> *names=qc.functionNames;
   id<MTLCommandQueue> queue=[dev newCommandQueue];
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget;
   id<MTLTexture> target=[dev newTextureWithDescriptor:td];
   id<MTLBuffer> zeros=[dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   unsigned n=0;
   for(NSString *name in names) {
    if(n>=pairs)break;
    id<MTLFunction> f=[qc newFunctionWithName:name];
    if(f.functionType!=MTLFunctionTypeVertex)continue;
    NSString *fragmentName=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
    if([fragmentName isEqualToString:name] || ![names containsObject:fragmentName])continue;
    ++n;
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=f;d.fragmentFunction=[qc newFunctionWithName:fragmentName];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    t=now();
    id<MTLRenderPipelineState> state=[dev newRenderPipelineStateWithDescriptor:d error:&error];
    const uint64_t took=(now()-t)/1000;
    if(!state){++refused;continue;}
    ++made;createUs+=took;createMax=std::max(createMax,took);
    t=now();
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLCommandBuffer> commands=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    for(NSUInteger i=0;i<8;++i){[encoder setVertexBuffer:zeros offset:0 atIndex:i];[encoder setFragmentBuffer:zeros offset:0 atIndex:i];}
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
    const uint64_t drew=(now()-t)/1000;
    drawUs+=drew;drawMax=std::max(drawMax,drew);
   }
  }
 }
 put(1,"Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",(unsigned long long)(libraryUs/1000),
  (unsigned long long)made,(unsigned long long)refused,(unsigned long long)(createUs/1000),(unsigned long long)(createMax/1000),
  (unsigned long long)(drawUs/1000),(unsigned long long)(drawMax/1000));
 size_t at=0;
 if(out && size) {
  std::lock_guard<std::mutex> lock(gBenchLock);
  out[0]=0;for(unsigned r=base;r<base+2;r++)at+=(size_t)snprintf(out+at,size>at?size-at:0,"%s\n",gBench[r]);
 }
 return at;
}
// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out,size_t size) {
 size_t at=0;out[0]=0;
 auto row=[&](const char *format,auto... args){
  char text[200];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>180)n=180;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 {std::lock_guard<std::mutex> lock(gBenchLock);for(unsigned r=0;r<4;r++)if(gBench[r][0])row("%.90s",gBench[r]);}
 row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),
  (unsigned long long)gCompleted.load(),(unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),
  (unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),
  (unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load());
 row("T pipeline n=%llu total=%llums max=%llums",(unsigned long long)gTimePipeline.count.load(),ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()));
 row("T library n=%llu total=%llums max=%llums",(unsigned long long)gTimeLibrary.count.load(),ms(gTimeLibrary.total.load()),ms(gTimeLibrary.slowest.load()));
 row("T function n=%llu total=%llums max=%llums",
  (unsigned long long)gTimeFunction.count.load(),ms(gTimeFunction.total.load()),ms(gTimeFunction.slowest.load()));
 row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",(unsigned long long)gTimeEncodeToCommit.count.load(),ms(gTimeEncodeToCommit.total.load()),
  ms(gTimeEncodeToCommit.slowest.load()),(unsigned long long)(gTimeEncodeToCommit.count.load()?gTimeEncodeToCommit.total.load()/gTimeEncodeToCommit.count.load():0));
 row("J scheduled wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitScheduled.count.load(),ms(gTimeWaitScheduled.total.load()),ms(gTimeWaitScheduled.slowest.load()));
 row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));
 row("J resource wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeResourceWait.count.load(),ms(gTimeResourceWait.total.load()),ms(gTimeResourceWait.slowest.load()));
 row("J resource checks=%llu skipped=%llu",(unsigned long long)gResourceChecks.load(),(unsigned long long)gResourceSkipped.load());
 row("J surface cache n=%llu bytes=%llu dup=%llu us=%llu max=%llu",(unsigned long long)gTimeSurfaceFlush.count.load(),
  (unsigned long long)gSurfaceFlushBytes.load(),(unsigned long long)gSurfaceFlushDuplicates.load(),
  (unsigned long long)gTimeSurfaceFlush.total.load(),(unsigned long long)gTimeSurfaceFlush.slowest.load());
 // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
 // its shader cache: where the driver's compile time goes.
 uint64_t work[10]={};
 if(gDevice && nmb_device_work_stats(gDevice,work)) {
  row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu",
   (unsigned long long)work[0],ms(work[1]),ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
  row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu",(unsigned long long)work[7],(unsigned long long)work[8],(unsigned long long)work[9],(unsigned long long)work[5],
   (unsigned long long)work[6]);
  char warning[200];size_t n=nmb_last_warning(warning,sizeof warning);
  if(n){row("W %.86s",warning);if(n>86)row("Wb %.86s",warning+86);}
 }
 {
  std::lock_guard<std::mutex> lock(gProblemLock);
  char formats[160];native_bridge_metal_formats_locked(formats,sizeof formats);row("F %.86s",formats);
  row("P0 %.86s",gLastProblems[0]);row("P0b %.86s",strlen(gLastProblems[0])>86?gLastProblems[0]+86:"");
  row("P1 %.86s",gLastProblems[1]);row("P1b %.86s",strlen(gLastProblems[1])>86?gLastProblems[1]+86:"");
 }
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  const uint32_t newest=(uint32_t)gCommits.load();
  row("%s","Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
  for(uint32_t k=0;k<20 && k<newest;k++) {
   const CommitRecord &r=gCommitRing[(newest-k)%32];
   if(r.index!=newest-k)continue;
   row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus",r.index,(unsigned long long)(r.committedUs/1000000),(unsigned long long)((r.committedUs/1000)%1000),
    r.work.passes,r.work.draws,(unsigned long long)(r.work.pixels>>10),r.work.dispatches,r.work.blits,(unsigned long long)r.callbackUs);
   if(k<4)row("E%u age=%lluus scheduled=%lldus",r.index,(unsigned long long)r.ageUs,(long long)r.scheduledUs);
  }
 }
 return at;
}

// 0258: the counters condensed for the in-kernel console: rows of at most 54 characters, one per line, so the
// 60-column screen dump fits a whole snapshot. Names: P/L/F pipeline-library-function creation, C commit-to-
// complete, Ws/Wc/Wr waits (scheduled, completed, resource), V the runtime's VkPipeline makings, K commits.
extern "C" size_t native_bridge_metal_short(char *out,size_t size) {
 size_t at=0;if(out&&size)out[0]=0;
 auto row=[&](const char *format,auto... args){
  char text[120];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>54)n=54;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 auto one=[&](const char *name,const Timed &t){
  row("%s n%llu mx%llu tot%llu",name,(unsigned long long)t.count.load(),ms(t.slowest.load()),ms(t.total.load()));};
 one("P",gTimePipeline);one("L",gTimeLibrary);one("F",gTimeFunction);one("C",gTimeEncodeToCommit);
 one("Ws",gTimeWaitScheduled);one("Wc",gTimeWaitCompleted);one("Wr",gTimeResourceWait);
 uint64_t work[10]={};
 if(gDevice&&nmb_device_work_stats(gDevice,work))
  row("V n%llu mx%llu w%llu f%llu",(unsigned long long)work[0],ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  const uint32_t newest=(uint32_t)gCommits.load();
  unsigned printed=0;
  for(uint32_t k=0;k<20&&k<newest&&printed<10;k++){
   const CommitRecord &r=gCommitRing[(newest-k)%32];
   if(r.index!=newest-k)continue;
   row("K%u t%llu ctc%llu age%llu rp%u dr%u",r.index,(unsigned long long)(r.committedUs/1000000),
    (unsigned long long)ms(r.callbackUs),(unsigned long long)ms(r.ageUs),r.work.passes,r.work.draws);
   printed++;
  }
 }
 return at;
}

extern "C" size_t native_bridge_metal_formats(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return native_bridge_metal_formats_locked(out,size);
}
