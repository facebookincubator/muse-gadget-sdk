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
import UIKit
import WebKit

/*
 * A web page on the gadget (protocols/UI.md): an offscreen WKWebView with a
 * square CSS viewport (the panel's size in CSS pixels by default, or a zoom
 * of it), snapshotted on change into the UI stream. WebKit renders only in a
 * window, so the view sits in the app's window behind everything else.
 * Changes come from a script in the page (a MutationObserver, scrolling,
 * animations: two animation frames later the page has painted), plus a
 * slow poll for what those miss (canvas, CSS animations); the stream's own
 * block compare drops snapshots that didn't change. Touches come back in
 * square coordinates and go into the page as pointer and mouse events, a
 * click for a tap and scrolling for a drag.
 */
final class UiWebSource: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
    let web: WKWebView
    private weak var streamer: UiStreamer?
    private(set) var viewport: CGFloat
    private var timer: Timer?
    private var dirtyUntilMs: Int64 = 0
    private var lastShotMs: Int64 = 0
    private var shooting = false
    private var pending = false
    var pollMs: Int64 = 250        /* when nothing reported a change */
    var busyMs: Int64 = 66         /* for a second after a change (15 a second) */
    var statSnapshots = 0, statSnapshotMs = 0.0
    var onLoaded: ((String) -> Void)?

    private static let script = """
    (function() {
      if (window.__mg) return; window.__mg = true;
      let pend = false;
      function dirty() {
        if (pend) return; pend = true;
        requestAnimationFrame(() => requestAnimationFrame(() => { pend = false;
          try { window.webkit.messageHandlers.mgDirty.postMessage(1); } catch (e) {} }));
      }
      new MutationObserver(dirty).observe(document, {subtree: true, childList: true, attributes: true, characterData: true});
      ['scroll', 'animationstart', 'animationiteration', 'transitionrun', 'transitionend', 'load', 'resize', 'input', 'focus']
        .forEach(e => addEventListener(e, dirty, true));
      function scroller(el) {
        for (let e = el; e && e !== document.documentElement; e = e.parentElement) {
          const s = getComputedStyle(e);
          if ((/(auto|scroll)/.test(s.overflowY) && e.scrollHeight > e.clientHeight) ||
              (/(auto|scroll)/.test(s.overflowX) && e.scrollWidth > e.clientWidth)) return e;
        }
        return document.scrollingElement || document.documentElement;
      }
      function fire(el, type, x, y, ctor) {
        if (!el) return;
        const o = {bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y, screenX: x, screenY: y,
                   button: 0, buttons: type.endsWith('up') || type === 'click' ? 0 : 1, view: window};
        try {
          el.dispatchEvent(ctor === 'p' ? new PointerEvent(type, Object.assign({pointerId: 1, pointerType: 'touch', isPrimary: true}, o))
                                       : new MouseEvent(type, o));
        } catch (e) {}
      }
      let st = null;
      window.__mgTouch = function(a, x, y) {
        if (a === 0) {
          const el = document.elementFromPoint(x, y);
          st = {x: x, y: y, lx: x, ly: y, el: el, drag: false, sc: scroller(el)};
          fire(el, 'pointerdown', x, y, 'p'); fire(el, 'mousedown', x, y);
          return el ? el.tagName : '';
        }
        if (!st) return '';
        if (a === 1) {
          if (!st.drag && Math.hypot(x - st.x, y - st.y) > 8) st.drag = true;
          if (st.drag) { st.sc.scrollBy(st.lx - x, st.ly - y); }
          fire(st.el, 'pointermove', x, y, 'p'); fire(st.el, 'mousemove', x, y);
          st.lx = x; st.ly = y;
          return st.drag ? 'drag' : '';
        }
        fire(st.el, 'pointerup', x, y, 'p'); fire(st.el, 'mouseup', x, y);
        let r = 'up';
        if (!st.drag && st.el) {
          fire(st.el, 'click', x, y);
          if (st.el.focus) st.el.focus();
          r = 'click ' + st.el.tagName;
        }
        st = null;
        return r;
      };
      dirty();
    })();
    """

    init(viewport: CGFloat) {
        self.viewport = viewport
        let cfg = WKWebViewConfiguration()
        cfg.suppressesIncrementalRendering = false
        cfg.allowsInlineMediaPlayback = true
        cfg.mediaTypesRequiringUserActionForPlayback = []
        let uc = WKUserContentController()
        uc.addUserScript(WKUserScript(source: Self.script, injectionTime: .atDocumentEnd, forMainFrameOnly: true))
        cfg.userContentController = uc
        web = WKWebView(frame: CGRect(x: 0, y: 0, width: viewport, height: viewport), configuration: cfg)
        super.init()
        uc.add(WeakHandler(self), name: "mgDirty")
        web.navigationDelegate = self
        web.isOpaque = true
        web.backgroundColor = .black
        web.scrollView.contentInsetAdjustmentBehavior = .never
        web.scrollView.showsVerticalScrollIndicator = false
        web.scrollView.showsHorizontalScrollIndicator = false
        web.isUserInteractionEnabled = false
    }

    /* Into the window, behind the app's own views (WebKit renders only in a window). */
    func attach(to window: UIWindow) {
        if web.superview !== window {
            window.insertSubview(web, at: 0)
        }
    }

    func setViewport(_ v: CGFloat) {
        viewport = v
        web.frame = CGRect(x: 0, y: 0, width: v, height: v)
        markDirty()
    }

    func load(_ s: String) {
        if let name = s.hasPrefix("demo:") ? String(s.dropFirst(5)) : nil,
           let url = Bundle.main.url(forResource: name, withExtension: "html", subdirectory: "ui") {
            web.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        } else if let url = URL(string: s.contains("://") ? s : "https://" + s) {
            web.load(URLRequest(url: url))
        }
        markDirty()
    }

    func start(_ s: UiStreamer) {
        streamer = s
        timer?.invalidate()
        let t = Timer(timeInterval: 0.016, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        markDirty()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        streamer = nil
    }

    func detach() {
        stop()
        web.removeFromSuperview()
    }

    func markDirty() {
        dirtyUntilMs = nowMs() + 1000
        pending = true
    }

    func userContentController(_ uc: WKUserContentController, didReceive m: WKScriptMessage) {
        markDirty()
    }

    func webView(_ w: WKWebView, didFinish n: WKNavigation!) {
        markDirty()
        onLoaded?(w.title ?? w.url?.absoluteString ?? "")
    }

    private func tick() {
        guard streamer != nil, !shooting else { return }
        let now = nowMs()
        let gap = pending || now < dirtyUntilMs ? busyMs : pollMs
        if now - lastShotMs < gap {
            return
        }
        pending = false
        shoot()
    }

    private func shoot() {
        shooting = true
        let t0 = nowNs()
        let cfg = WKSnapshotConfiguration()
        cfg.rect = CGRect(x: 0, y: 0, width: viewport, height: viewport)
        cfg.afterScreenUpdates = false
        lastShotMs = nowMs()
        web.takeSnapshot(with: cfg) { [weak self] img, err in
            guard let self else { return }
            self.shooting = false
            guard let cg = img?.cgImage, let s = self.streamer else {
                if let e = err {
                    mlog("ui: snapshot failed: \(e.localizedDescription)")
                }
                return
            }
            let capNs = t0
            self.statSnapshots += 1
            self.statSnapshotMs += Double(nowNs() - t0) / 1e6
            guard let pb = Self.pixelBuffer(cg) else { return }
            DispatchQueue.global(qos: .userInteractive).async {
                s.submit(pb, crop: SIMD4<Float>(0, 0, 1, 1), captureNs: capNs)
            }
        }
    }

    /* A touch in square coordinates (0..1): into the page. */
    func touch(action: Int, sx: Double, sy: Double) {
        let x = sx * Double(viewport), y = sy * Double(viewport)
        web.evaluateJavaScript(String(format: "window.__mgTouch && window.__mgTouch(%d, %.1f, %.1f)", action, x, y)) { r, _ in
            if let s = r as? String, !s.isEmpty, action != 1 || s == "drag" && Int.random(in: 0..<10) == 0 {
                mlog("ui: touch \(action) at \(Int(x)),\(Int(y)) css: \(s)")
            }
        }
        markDirty()
    }

    static func pixelBuffer(_ cg: CGImage) -> CVPixelBuffer? {
        let w = cg.width, h = cg.height
        var pb: CVPixelBuffer?
        let attrs: [CFString: Any] = [kCVPixelBufferMetalCompatibilityKey: true, kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb) == kCVReturnSuccess,
              let buf = pb else { return nil }
        CVPixelBufferLockBaseAddress(buf, [])
        defer { CVPixelBufferUnlockBaseAddress(buf, []) }
        guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buf), width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: CVPixelBufferGetBytesPerRow(buf), space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        return buf
    }
}

/* WKUserContentController keeps its handlers strongly. */
private final class WeakHandler: NSObject, WKScriptMessageHandler {
    weak var target: WKScriptMessageHandler?

    init(_ t: WKScriptMessageHandler) {
        target = t
    }

    func userContentController(_ uc: WKUserContentController, didReceive m: WKScriptMessage) {
        target?.userContentController(uc, didReceive: m)
    }
}
