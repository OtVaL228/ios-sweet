/*
 * vtprobe: whether a file's video track decodes in this process on guacamole (no mediaserverd, no
 * AVPlayer). iOS exports no VTCopyVideoDecoderList; feeds the compressed samples of the first video track (AVAssetReader, passthrough) to a VTDecompressionSession
 * and prints the decoder picked, every error and the decode rate. MIT, as userland/.
 *
 *   vtprobe                 hardware decode support only
 *   vtprobe FILE [N] [sw]   decode up to N frames (default 120); "sw" asks for a software decoder
 */
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>

static const char *
fourcc(uint32_t t, char buf[8])
{
	buf[0] = t >> 24; buf[1] = t >> 16; buf[2] = t >> 8; buf[3] = t; buf[4] = 0;
	return buf;
}

static int nframes, nerrors;
static OSStatus lasterr;

static void
output(void *refcon, void *frame, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef img,
    CMTime pts, CMTime dur)
{
	char b[8];
	if (status != noErr || img == NULL) {
		if (nerrors++ < 5)
			printf("vtprobe: frame error %d flags 0x%x\n", (int)status, (unsigned)flags);
		lasterr = status;
		return;
	}
	if (nframes++ == 0) {
		CVPixelBufferRef pb = (CVPixelBufferRef)img;
		printf("vtprobe: first frame %zux%zu %s iosurface %s\n", CVPixelBufferGetWidth(pb),
		    CVPixelBufferGetHeight(pb), fourcc(CVPixelBufferGetPixelFormatType(pb), b),
		    CVPixelBufferGetIOSurface(pb) ? "yes" : "no");
	}
}

static void
list_decoders(void)
{
	for (uint32_t t = 0; t < 2; t++) {
		CMVideoCodecType c = t ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
		char b[8];
		printf("vtprobe: VTIsHardwareDecodeSupported(%s) = %d\n", fourcc(c, b), VTIsHardwareDecodeSupported(c));
	}
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		list_decoders();
		if (argc < 2)
			return 0;
		int want = argc > 2 ? atoi(argv[2]) : 120;
		bool sw = argc > 3 && strcmp(argv[3], "sw") == 0;

		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
		NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
		if (tracks.count == 0) {
			printf("vtprobe: no video track\n");
			return 1;
		}
		AVAssetTrack *track = tracks[0];
		NSError *e = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&e];
		AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
		[r addOutput:out];
		if (![r startReading]) {
			printf("vtprobe: startReading: %s\n", [[r.error description] UTF8String]);
			return 1;
		}
		CMFormatDescriptionRef fmt = (__bridge CMFormatDescriptionRef)track.formatDescriptions[0];
		CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);
		char b[8];
		printf("vtprobe: track %s %dx%d %.1f fps\n", fourcc(CMFormatDescriptionGetMediaSubType(fmt), b),
		    dim.width, dim.height, track.nominalFrameRate);

		NSDictionary *spec = sw ? @{ @"EnableHardwareAcceleratedVideoDecoder": @NO } : nil;
		NSDictionary *attrs = @{ (__bridge id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
		VTDecompressionOutputCallbackRecord cb = { output, NULL };
		VTDecompressionSessionRef s = NULL;
		OSStatus err = VTDecompressionSessionCreate(NULL, fmt, (__bridge CFDictionaryRef)spec,
		    (__bridge CFDictionaryRef)attrs, &cb, &s);
		printf("vtprobe: VTDecompressionSessionCreate = %d\n", (int)err);
		if (err)
			return 1;
		CFBooleanRef hw = NULL;
		VTSessionCopyProperty(s, CFSTR("UsingHardwareAcceleratedVideoDecoder"), NULL, &hw);
		printf("vtprobe: hardware decoder: %s\n", hw == kCFBooleanTrue ? "yes" : hw ? "no" : "?");

		uint64_t t0 = mach_absolute_time();
		int fed = 0;
		CMSampleBufferRef sb;
		while (fed < want && (sb = [out copyNextSampleBuffer]) != NULL) {
			if (CMSampleBufferGetNumSamples(sb) > 0) {
				err = VTDecompressionSessionDecodeFrame(s, sb, 0, NULL, NULL);
				if (err && nerrors++ < 5)
					printf("vtprobe: DecodeFrame %d = %d\n", fed, (int)err);
				fed++;
			}
			CFRelease(sb);
		}
		VTDecompressionSessionWaitForAsynchronousFrames(s);
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		double secs = (mach_absolute_time() - t0) * (double)tb.numer / tb.denom / 1e9;
		printf("vtprobe: fed %d, decoded %d, errors %d (last %d), %.2f s, %.1f fps\n", fed, nframes, nerrors,
		    (int)lasterr, secs, secs > 0 ? nframes / secs : 0);
		VTDecompressionSessionInvalidate(s);
		CFRelease(s);
	}
	return 0;
}
