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

import CoreVideo
import Foundation
import Metal

/*
 * The UI picture on the GPU (protocols/UI.md): a square of a source picture
 * (a web page's snapshot, the screen) to the gadget's panel size, optionally
 * through the square-to-disc warp, filtered (4x4 samples a pixel, each
 * through the warp, so text scaled down stays smooth), masked to the round
 * panel, and out as I420 (BT.601, video range: what the gadgets' decoders
 * show) for the encoder, plus BGRA for the phone's preview. The shader is
 * compiled from source at run time, so the broadcast extension needs no
 * metallib of its own.
 */
final class UiWarp {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeRgb: MTLComputePipelineState, pipeNv12: MTLComputePipelineState
    private var cache: CVMetalTextureCache?
    private var yBuf: MTLBuffer?, uBuf: MTLBuffer?, vBuf: MTLBuffer?, rgbBuf: MTLBuffer?
    private var n = 0
    /* One render at a time: the buffers are shared by every stream that uses this warp. */
    private let renderLock = NSLock()
    private let sampler: MTLSamplerState

    struct Params {
        var crop: SIMD4<Float>   /* the source square: x, y, w, h in 0..1 of the source */
        var n: UInt32            /* output size (square) */
        var warp: UInt32         /* 1: square to disc */
        var mask: UInt32         /* 1: black outside the inscribed circle */
        var ss: UInt32           /* samples a side per pixel */
        var edge: Float = 0      /* past the source's edges: a faint line this wide (source fractions), then black */
    }

    private static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct P { float4 crop; uint n; uint warp; uint mask; uint ss; float edge; };

    // protocols/UI.md: square from disc (the elliptical grid mapping's inverse)
    static float2 sq_from_disc(float2 d) {
        float u = d.x, v = d.y;
        float r2 = u * u + v * v;
        if (r2 > 1.0) { float r = sqrt(r2); u /= r; v /= r; }
        const float s8 = 2.0 * sqrt(2.0);
        float uu = u * u, vv = v * v;
        float x = 0.5 * sqrt(max(0.0, 2.0 + uu - vv + s8 * u)) - 0.5 * sqrt(max(0.0, 2.0 + uu - vv - s8 * u));
        float y = 0.5 * sqrt(max(0.0, 2.0 - uu + vv + s8 * v)) - 0.5 * sqrt(max(0.0, 2.0 - uu + vv - s8 * v));
        return float2(clamp(x, -1.0, 1.0), clamp(y, -1.0, 1.0));
    }

    static float2 src_uv(constant P &p, float2 d) {
        float2 s = p.warp != 0 ? sq_from_disc(d) : d;
        return p.crop.xy + (s * 0.5 + 0.5) * p.crop.zw;
    }

    // past the source's edges (a mirror window panned past the screen): black, with a faint line at the edge
    static bool outside(constant P &p, float2 uv, thread float3 &c) {
        float2 o2 = max(-uv, uv - 1.0);
        float o = max(o2.x, o2.y);
        if (o <= 0.0) return false;
        c = o < p.edge ? float3(0.28) : float3(0.0);
        return true;
    }

    // one pixel's colour: ss x ss samples across it, each through the warp (FETCH(uv) -> float3)
    #define PIXEL(OUT, PX, FETCH) { \
        float n_ = float(p.n); \
        float2 c_ = (float2(PX) + 0.5) * 2.0 / n_ - 1.0; \
        float lim_ = 1.0 + 1.5 / n_; \
        if (p.mask != 0 && dot(c_, c_) > lim_ * lim_) { OUT = float3(0.0); } else { \
            float3 acc_ = float3(0.0); \
            for (uint j_ = 0; j_ < p.ss; j_++) for (uint i_ = 0; i_ < p.ss; i_++) { \
                float2 q_ = float2(PX) + (float2(i_, j_) + 0.5) / float(p.ss); \
                float2 uv = src_uv(p, q_ * 2.0 / n_ - 1.0); \
                float3 oc_; \
                acc_ += outside(p, uv, oc_) ? oc_ : FETCH; \
            } \
            OUT = acc_ / float(p.ss * p.ss); } }

    static float3 fetch_nv12(texture2d<float> ty, texture2d<float> tc, sampler s, float2 uv) {
        float yy = ty.sample(s, uv).r;
        float2 c = tc.sample(s, uv).rg - 0.5;
        return float3(yy + 1.5748 * c.y, yy - 0.1873 * c.x - 0.4681 * c.y, yy + 1.8556 * c.x);
    }

    static void store(constant P &p, uint2 g, thread float3 *rgb, device uchar *y, device uchar *u, device uchar *v,
                      device uchar4 *bgra) {
        uint n = p.n;
        float su = 0.0, sv = 0.0;
        for (uint k = 0; k < 4; k++) {
            uint2 px = g * 2 + uint2(k & 1, k >> 1);
            float3 c = clamp(rgb[k], 0.0, 1.0);
            float yy = 16.0 + 65.481 * c.r + 128.553 * c.g + 24.966 * c.b;
            su += -37.797 * c.r - 74.203 * c.g + 112.0 * c.b;
            sv += 112.0 * c.r - 93.786 * c.g - 18.214 * c.b;
            y[px.y * n + px.x] = uchar(clamp(round(yy), 16.0, 235.0));
            if (bgra) bgra[px.y * n + px.x] = uchar4(uchar(round(c.b * 255.0)), uchar(round(c.g * 255.0)), uchar(round(c.r * 255.0)), 255);
        }
        uint h = n / 2;
        u[g.y * h + g.x] = uchar(clamp(round(128.0 + su / 4.0), 16.0, 240.0));
        v[g.y * h + g.x] = uchar(clamp(round(128.0 + sv / 4.0), 16.0, 240.0));
    }

    kernel void warp_rgb(texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]], constant P &p [[buffer(0)]],
                         device uchar *y [[buffer(1)]], device uchar *u [[buffer(2)]], device uchar *v [[buffer(3)]],
                         device uchar4 *bgra [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
        if (g.x * 2 >= p.n || g.y * 2 >= p.n) return;
        float3 rgb[4];
        for (uint k = 0; k < 4; k++) { uint2 px = g * 2 + uint2(k & 1, k >> 1); PIXEL(rgb[k], px, src.sample(s, uv).rgb) }
        store(p, g, rgb, y, u, v, bgra);
    }

    // ReplayKit's 420f (full range, BT.709)
    kernel void warp_nv12(texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]], sampler s [[sampler(0)]],
                          constant P &p [[buffer(0)]], device uchar *y [[buffer(1)]], device uchar *u [[buffer(2)]],
                          device uchar *v [[buffer(3)]], device uchar4 *bgra [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
        if (g.x * 2 >= p.n || g.y * 2 >= p.n) return;
        float3 rgb[4];
        for (uint k = 0; k < 4; k++) { uint2 px = g * 2 + uint2(k & 1, k >> 1); PIXEL(rgb[k], px, fetch_nv12(ty, tc, s, uv)) }
        store(p, g, rgb, y, u, v, bgra);
    }
    """

    init?() {
        guard let d = MTLCreateSystemDefaultDevice(), let q = d.makeCommandQueue() else { return nil }
        device = d
        queue = q
        do {
            let opts = MTLCompileOptions()
            opts.languageVersion = .version3_0
            let lib = try d.makeLibrary(source: Self.source, options: opts)
            pipeRgb = try d.makeComputePipelineState(function: lib.makeFunction(name: "warp_rgb")!)
            pipeNv12 = try d.makeComputePipelineState(function: lib.makeFunction(name: "warp_nv12")!)
        } catch {
            mlog("ui: Metal shader: \(error)")
            return nil
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear
        sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge
        sd.tAddressMode = .clampToEdge
        sampler = d.makeSamplerState(descriptor: sd)!
        CVMetalTextureCacheCreate(nil, nil, d, nil, &cache)
    }

    private func buffers(_ size: Int, preview: Bool) {
        if n == size && (rgbBuf != nil || !preview) {
            return
        }
        n = size
        yBuf = device.makeBuffer(length: size * size, options: .storageModeShared)
        uBuf = device.makeBuffer(length: size * size / 4, options: .storageModeShared)
        vBuf = device.makeBuffer(length: size * size / 4, options: .storageModeShared)
        rgbBuf = preview ? device.makeBuffer(length: size * size * 4, options: .storageModeShared) : nil
    }

    private func texture(_ pb: CVPixelBuffer, plane: Int, _ fmt: MTLPixelFormat) -> (MTLTexture, CVMetalTexture)? {
        guard let c = cache else { return nil }
        let w = CVPixelBufferGetWidthOfPlane(pb, plane), h = CVPixelBufferGetHeightOfPlane(pb, plane)
        var t: CVMetalTexture?
        guard CVMetalTextureCacheCreateTextureFromImage(nil, c, pb, nil, fmt, w, h, plane, &t) == kCVReturnSuccess,
              let tt = t, let mt = CVMetalTextureGetTexture(tt) else { return nil }
        return (mt, tt)
    }

    /*
     * The crop (x, y, w, h in 0..1 of the source; the caller keeps it square in
     * pixels) of pb (BGRA, or NV12 420f/420v) to an n x n I420 picture in
     * i420 (n*n*3/2 bytes), and BGRA into preview when given. Waits for the GPU.
     */
    func render(_ pb: CVPixelBuffer, crop: SIMD4<Float>, n: Int, warp: Bool, mask: Bool,
                i420: UnsafeMutablePointer<UInt8>, preview: UnsafeMutablePointer<UInt8>? = nil) -> Bool {
        renderLock.lock()
        defer { renderLock.unlock() }
        buffers(n, preview: preview != nil)
        guard let cb = queue.makeCommandBuffer(), let enc = cb.makeComputeCommandEncoder() else { return false }
        let fmt = CVPixelBufferGetPixelFormatType(pb)
        var keep: [CVMetalTexture] = []
        if fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange {
            guard let (ty, a) = texture(pb, plane: 0, .r8Unorm), let (tc, b) = texture(pb, plane: 1, .rg8Unorm) else {
                enc.endEncoding()
                return false
            }
            keep = [a, b]
            enc.setComputePipelineState(pipeNv12)
            enc.setTexture(ty, index: 0)
            enc.setTexture(tc, index: 1)
        } else {
            guard let (t, a) = texture(pb, plane: 0, .bgra8Unorm) else {
                enc.endEncoding()
                return false
            }
            keep = [a]
            enc.setComputePipelineState(pipeRgb)
            enc.setTexture(t, index: 0)
        }
        /* the edge line: about 1.5 output pixels, in source fractions */
        var p = Params(crop: crop, n: UInt32(n), warp: warp ? 1 : 0, mask: mask ? 1 : 0, ss: 4,
                       edge: 1.5 * max(crop.z, crop.w) / Float(max(1, n)))
        enc.setSamplerState(sampler, index: 0)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.setBuffer(yBuf, offset: 0, index: 1)
        enc.setBuffer(uBuf, offset: 0, index: 2)
        enc.setBuffer(vBuf, offset: 0, index: 3)
        enc.setBuffer(rgbBuf, offset: 0, index: 4)
        let tg = MTLSize(width: 8, height: 8, depth: 1)
        enc.dispatchThreads(MTLSize(width: n / 2, height: n / 2, depth: 1), threadsPerThreadgroup: tg)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        _ = keep
        guard cb.status == .completed, let y = yBuf, let u = uBuf, let v = vBuf else { return false }
        memcpy(i420, y.contents(), n * n)
        memcpy(i420 + n * n, u.contents(), n * n / 4)
        memcpy(i420 + n * n * 5 / 4, v.contents(), n * n / 4)
        if let pv = preview, let r = rgbBuf {
            memcpy(pv, r.contents(), n * n * 4)
        }
        return true
    }
}

/* The warp on the CPU, for touches (protocols/UI.md). */
enum UiGeometry {
    /* A panel pixel to the square, both as 0..1 from the top-left corner. */
    static func squareFromPanel(px: Double, py: Double, n: Int, warp: Bool) -> (Double, Double) {
        var u = (2 * px + 1) / Double(n) - 1, v = (2 * py + 1) / Double(n) - 1
        if warp {
            let r2 = u * u + v * v
            if r2 > 1 {
                let r = r2.squareRoot()
                u /= r
                v /= r
            }
            let s8 = 2 * 2.0.squareRoot(), uu = u * u, vv = v * v
            let x = 0.5 * max(0, 2 + uu - vv + s8 * u).squareRoot() - 0.5 * max(0, 2 + uu - vv - s8 * u).squareRoot()
            let y = 0.5 * max(0, 2 - uu + vv + s8 * v).squareRoot() - 0.5 * max(0, 2 - uu + vv - s8 * v).squareRoot()
            u = x
            v = y
        }
        return (min(1, max(0, (u + 1) / 2)), min(1, max(0, (v + 1) / 2)))
    }

    /* The square (0..1) to the disc (0..1 of the panel): the forward warp. */
    static func panelFromSquare(sx: Double, sy: Double, warp: Bool) -> (Double, Double) {
        let x = sx * 2 - 1, y = sy * 2 - 1
        if !warp {
            return (sx, sy)
        }
        let u = x * (1 - y * y / 2).squareRoot(), v = y * (1 - x * x / 2).squareRoot()
        return ((u + 1) / 2, (v + 1) / 2)
    }
}
