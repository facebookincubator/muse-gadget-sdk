/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/* A software encoder: one I420 picture (w*h luma, then the quarter planes) in, one frame out. */
protocol SwEncoder: AnyObject {
    /* nil i420: flush one frame a lookahead holds (x264 clips). */
    func encode(_ i420: UnsafePointer<UInt8>?, pts: Int64, key: Bool) -> (data: Data, key: Bool, qp: Int)?
    func setBitrate(_ kbps: Int)
    func close()
}

/* libx264 through native/mgx264.c, when the app was built with it (native/build_x264.sh). */
final class X264: SwEncoder {
    static var available: Bool { mgx264_available() != 0 }
    private var h: UnsafeMutableRawPointer?

    init?(w: Int, h: Int, fps: Int, kbps: Int, preset: String, params: String?) {
        self.h = mgx264_open(Int32(w), Int32(h), Int32(fps), Int32(kbps), preset, params)
        if self.h == nil {
            return nil
        }
    }

    func encode(_ i420: UnsafePointer<UInt8>?, pts: Int64, key: Bool) -> (data: Data, key: Bool, qp: Int)? {
        var out: UnsafePointer<UInt8>?
        var info: [Int32] = [0, 0]
        let n = mgx264_encode(h, i420, pts, key ? 1 : 0, &out, &info)
        guard n > 0, let o = out else { return nil }
        return (Data(bytes: o, count: Int(n)), info[0] != 0, Int(info[1]))
    }

    func setBitrate(_ kbps: Int) {
        mgx264_set_bitrate(h, Int32(kbps))
    }

    func close() {
        mgx264_close(h)
        h = nil
    }
}

/*
 * libvpx's VP8 encoder through native/mgvpx.c (native/build_vpx.sh). params
 * "name=value:..." (defaults in brackets): cpu [2], deadline [good] (good or
 * rt), profile [1], lf [0], mvcheap [1], threads [1], vbv-ms [200], vbr [0].
 */
final class Vpx: SwEncoder {
    static var available: Bool { mgvpx_available() != 0 }
    private var h: UnsafeMutableRawPointer?

    init?(w: Int, h: Int, fps: Int, kbps: Int, params: String?) {
        self.h = mgvpx_open(Int32(w), Int32(h), Int32(fps), Int32(kbps), params)
        if self.h == nil {
            return nil
        }
    }

    func encode(_ i420: UnsafePointer<UInt8>?, pts: Int64, key: Bool) -> (data: Data, key: Bool, qp: Int)? {
        guard let p = i420 else { return nil }
        var out: UnsafePointer<UInt8>?
        var info: [Int32] = [0, 0]
        let n = mgvpx_encode(h, p, pts, key ? 1 : 0, &out, &info)
        guard n > 0, let o = out else { return nil }
        return (Data(bytes: o, count: Int(n)), info[0] != 0, Int(info[1]))
    }

    func setBitrate(_ kbps: Int) {
        mgvpx_set_bitrate(h, Int32(kbps))
    }

    func close() {
        mgvpx_close(h)
        h = nil
    }
}

/*
 * The phone's H.264 encoder (VideoToolbox): constrained baseline, no frame
 * reordering, keyframes only when asked, Annex B out with SPS and PPS
 * before every keyframe. Live: low-latency rate control (CBR-like, a frame
 * out for each in at once). Clips: VBR, frames out in order at the end.
 * Like Android's hardware path it always deblocks (the gadget's decoder
 * does ~30% more work than with x264's no-deblock) and has no intra
 * refresh; x264 (native/build_x264.sh) is better where the app has it.
 */
final class VTH264 {
    typealias Output = (_ au: Data, _ key: Bool, _ index: Int64) -> Void
    private var session: VTCompressionSession?
    private let w: Int, h: Int
    private let output: Output
    private var pool: CVPixelBufferPool?
    var name = "VideoToolbox H.264"

    init?(w: Int, h: Int, fps: Int, kbps: Int, live: Bool, output: @escaping Output) {
        self.w = w
        self.h = h
        self.output = output
        var spec: [CFString: Any] = [:]
        if live {
            spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true
        }
        var s: VTCompressionSession?
        func create(_ spec: [CFString: Any]) -> OSStatus {
            VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264,
                                       encoderSpecification: spec as CFDictionary,
                                       imageBufferAttributes: [
                                           kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                           kCVPixelBufferWidthKey: w, kCVPixelBufferHeightKey: h,
                                       ] as CFDictionary,
                                       compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        }
        var rc = create(spec)
        if rc != noErr && live {
            mlog("VideoToolbox: no low-latency H.264 encoder (\(rc)); the ordinary one")
            rc = create([:])
            name = "VideoToolbox H.264 (not low-latency)"
        }
        guard rc == noErr, let sess = s else {
            mlog("VideoToolbox: no H.264 encoder (\(rc))")
            return nil
        }
        session = sess
        func set(_ k: CFString, _ v: Any) {
            let e = VTSessionSetProperty(sess, key: k, value: v as CFTypeRef)
            if e != noErr {
                mlog("VideoToolbox: \(k) not set (\(e))")
            }
        }
        set(kVTCompressionPropertyKey_RealTime, live)
        if VTSessionSetProperty(sess, key: kVTCompressionPropertyKey_ProfileLevel,
                                value: kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel) != noErr {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel)
        }
        set(kVTCompressionPropertyKey_AllowFrameReordering, false)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, 100_000)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 100_000)
        set(kVTCompressionPropertyKey_AverageBitRate, kbps * 1000)
        if live {
            /* at most the rate over any 200 ms */
            set(kVTCompressionPropertyKey_DataRateLimits, [kbps * 1000 / 8 / 5, 0.2] as CFArray)
        }
        VTCompressionSessionPrepareToEncodeFrames(sess)
        pool = VTCompressionSessionGetPixelBufferPool(sess)
    }

    func setBitrate(_ kbps: Int) {
        guard let s = session else { return }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: (kbps * 1000) as CFTypeRef)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits,
                             value: [kbps * 1000 / 8 / 5, 0.2] as CFArray)
    }

    /* One I420 picture; index comes back with its output. */
    func encode(i420: UnsafePointer<UInt8>, index: Int64, fps: Int, key: Bool) {
        guard let s = session else { return }
        var pb: CVPixelBuffer?
        if let p = pool {
            CVPixelBufferPoolCreatePixelBuffer(nil, p, &pb)
        }
        if pb == nil {
            CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nil, &pb)
        }
        guard let buf = pb else { return }
        CVPixelBufferLockBaseAddress(buf, [])
        let yd = CVPixelBufferGetBaseAddressOfPlane(buf, 0)!.assumingMemoryBound(to: UInt8.self)
        let ys = CVPixelBufferGetBytesPerRowOfPlane(buf, 0)
        for y in 0..<h {
            memcpy(yd + y * ys, i420 + y * w, w)
        }
        let cd = CVPixelBufferGetBaseAddressOfPlane(buf, 1)!.assumingMemoryBound(to: UInt8.self)
        let cs = CVPixelBufferGetBytesPerRowOfPlane(buf, 1)
        let u = i420 + w * h, v = u + (w / 2) * (h / 2)
        for y in 0..<(h / 2) {
            let row = cd + y * cs
            for x in 0..<(w / 2) {
                row[2 * x] = u[y * (w / 2) + x]
                row[2 * x + 1] = v[y * (w / 2) + x]
            }
        }
        CVPixelBufferUnlockBaseAddress(buf, [])
        let pts = CMTime(value: index, timescale: CMTimeScale(max(1, fps)))
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: props, infoFlagsOut: nil) { [weak self] st, _, sb in
            guard let self, st == noErr, let sb else { return }
            self.deliver(sb)
        }
    }

    /* Clips: waits for every frame given so far. */
    func finish() {
        if let s = session {
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
        }
    }

    func close() {
        if let s = session {
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(s)
        }
        session = nil
    }

    private func deliver(_ sb: CMSampleBuffer) {
        guard let bb = CMSampleBufferGetDataBuffer(sb) else { return }
        var key = true
        if let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
           let a = att.first, let notSync = a[kCMSampleAttachmentKey_NotSync] as? Bool {
            key = !notSync
        }
        var out = Data()
        let sc: [UInt8] = [0, 0, 0, 1]
        if key, let fd = CMSampleBufferGetFormatDescription(sb) {
            var count = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                               parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                               nalUnitHeaderLengthOut: nil)
            for i in 0..<count {
                var p: UnsafePointer<UInt8>?
                var n = 0
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                                      parameterSetSizeOut: &n, parameterSetCountOut: nil,
                                                                      nalUnitHeaderLengthOut: nil) == noErr, let p {
                    out.append(contentsOf: sc)
                    out.append(p, count: n)
                }
            }
        }
        var len = 0
        var ptr: UnsafeMutablePointer<CChar>?
        guard CMBlockBufferGetDataPointer(bb, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len,
                                          dataPointerOut: &ptr) == noErr, let base = ptr else { return }
        /* AVCC (4-byte lengths) to Annex B */
        base.withMemoryRebound(to: UInt8.self, capacity: len) { b in
            var i = 0
            while i + 4 <= len {
                let n = Int(b[i]) << 24 | Int(b[i + 1]) << 16 | Int(b[i + 2]) << 8 | Int(b[i + 3])
                i += 4
                if n <= 0 || i + n > len {
                    break
                }
                out.append(contentsOf: sc)
                out.append(b + i, count: n)
                i += n
            }
        }
        output(out, key, CMSampleBufferGetPresentationTimeStamp(sb).value)
    }

    /* Whether an Annex B access unit holds an IDR slice. */
    static func isIdr(_ au: Data) -> Bool {
        var i = au.startIndex
        let e = au.endIndex
        while i + 3 < e {
            if au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1 {
                let t = au[i + 3] & 0x1f
                if t == 5 {
                    return true
                }
                if t == 1 {
                    return false
                }
            }
            i += 1
        }
        return false
    }
}
