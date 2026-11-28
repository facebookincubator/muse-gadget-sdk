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

/*
 * The UI stream's H.264 encoder for one band: a forced quantizer per frame
 * (progressive refinement), one reference, no B frames, keyframes only when
 * asked. x264 where the app has it (no deblocking, the reconstruction's
 * PSNR), else the phone's encoder with the frame QP pinned by its min and
 * max QP properties.
 */
protocol UiCodec: AnyObject {
    var name: String { get }
    /* One I420 picture; nil when the encoder produced nothing. psnr: the reconstruction's luma PSNR (0: unknown). */
    func encode(_ i420: UnsafePointer<UInt8>, qp: Int, idr: Bool) -> (au: Data, psnr: Double)?
    /* The picture the last frame decodes to (I420): what the gadget's decoder has. False: unknown. */
    func recon(_ dst: UnsafeMutablePointer<UInt8>) -> Bool
    func close()
}

extension UiCodec {
    func recon(_ dst: UnsafeMutablePointer<UInt8>) -> Bool { false }
}

final class UiX264: UiCodec {
    let name = "x264"
    private var h: UnsafeMutableRawPointer?
    private var pts: Int64 = 0

    init?(w: Int, h: Int, fps: Int) {
        /* CRF with each frame's QP forced (in constant-QP mode x264 clamps a
         * forced QP), no AQ or mbtree, so the forced QP is the QP; no fast
         * P-skip or DCT decimation, so refinements code small residuals;
         * keyint infinite and no scenecut: keyframes only when asked; no intra
         * refresh (Data is reliable); no deblocking (cheaper to decode, text
         * stays crisp); psnr: the reconstruction's quality for the stats. */
        let params = "crf=30:aq-mode=0:mbtree=0:no-fast-pskip=1:no-dct-decimate=1:keyint=infinite:scenecut=0"
            + ":intra-refresh=0:no-deblock=1:ref=1:bframes=0:psnr=1:full-recon=1"
        self.h = mgx264_open(Int32(w), Int32(h), Int32(fps), 1000, "fast", params)
        if self.h == nil {
            return nil
        }
    }

    func encode(_ i420: UnsafePointer<UInt8>, qp: Int, idr: Bool) -> (au: Data, psnr: Double)? {
        var out: UnsafePointer<UInt8>?
        var info: [Int32] = [0, 0]
        var psnr = 0.0
        pts += 1
        let n = mgx264_encode_qp(h, i420, pts, idr ? 1 : 0, Int32(qp), &out, &info, &psnr)
        guard n > 0, let o = out else { return nil }
        return (Data(bytes: o, count: Int(n)), psnr)
    }

    func recon(_ dst: UnsafeMutablePointer<UInt8>) -> Bool {
        mgx264_recon_i420(h, dst) != 0
    }

    func close() {
        mgx264_close(h)
        h = nil
    }
}

final class UiVideoToolbox: UiCodec {
    let name = "VideoToolbox"
    private var session: VTCompressionSession?
    private let w: Int, h: Int
    private var index: Int64 = 0
    private var result: Data?
    private var lastQp = -1

    init?(w: Int, h: Int, fps: Int) {
        self.w = w
        self.h = h
        var s: VTCompressionSession?
        let rc = VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264,
                                            encoderSpecification: nil,
                                            imageBufferAttributes: [
                                                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                                kCVPixelBufferWidthKey: w, kCVPixelBufferHeightKey: h,
                                            ] as CFDictionary,
                                            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard rc == noErr, let sess = s else {
            mlog("ui: no VideoToolbox H.264 encoder (\(rc))")
            return nil
        }
        session = sess
        func set(_ k: CFString, _ v: Any) {
            let e = VTSessionSetProperty(sess, key: k, value: v as CFTypeRef)
            if e != noErr {
                mlog("ui: VideoToolbox \(k) not set (\(e))")
            }
        }
        set(kVTCompressionPropertyKey_RealTime, true)
        if VTSessionSetProperty(sess, key: kVTCompressionPropertyKey_ProfileLevel,
                                value: kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel) != noErr {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel)
        }
        set(kVTCompressionPropertyKey_AllowFrameReordering, false)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, 1_000_000)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 1_000_000)
        set(kVTCompressionPropertyKey_ReferenceBufferCount, 1)
        VTCompressionSessionPrepareToEncodeFrames(sess)
    }

    func encode(_ i420: UnsafePointer<UInt8>, qp: Int, idr: Bool) -> (au: Data, psnr: Double)? {
        guard let s = session else { return nil }
        if qp != lastQp {
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MinAllowedFrameQP, value: qp as CFTypeRef)
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxAllowedFrameQP, value: qp as CFTypeRef)
            lastQp = qp
        }
        var pb: CVPixelBuffer?
        CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nil, &pb)
        guard let buf = pb else { return nil }
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
        index += 1
        result = nil
        let props: CFDictionary? = idr ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: CMTime(value: index, timescale: 15),
                                        duration: .invalid, frameProperties: props, infoFlagsOut: nil) { [weak self] st, _, sb in
            guard let self, st == noErr, let sb else { return }
            self.result = Self.annexB(sb)
        }
        VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
        guard let r = result else { return nil }
        return (r, 0)
    }

    func close() {
        if let s = session {
            VTCompressionSessionInvalidate(s)
        }
        session = nil
    }

    static func annexB(_ sb: CMSampleBuffer) -> Data? {
        guard let bb = CMSampleBufferGetDataBuffer(sb) else { return nil }
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
                                          dataPointerOut: &ptr) == noErr, let base = ptr else { return nil }
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
        return out
    }
}
