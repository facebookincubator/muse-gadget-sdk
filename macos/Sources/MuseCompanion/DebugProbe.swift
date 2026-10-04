// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import AppKit
import SceneKit

/// Opt-in development probes capture only this application's own window.
/// They are compiled out of the release app.
enum DebugProbe {
    static func snapshot() {
        #if DEBUG
        guard let path = ProcessInfo.processInfo.environment["MUSE_TEST_SNAPSHOT"],
              let content = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil })?.contentView,
              let bitmap = content.bitmapImageRepForCachingDisplay(in: content.bounds) else { return }
        content.cacheDisplay(in: content.bounds, to: bitmap)
        func scenes(_ view: NSView) -> [SCNView] {
            (view as? SCNView).map { [$0] } ?? view.subviews.flatMap(scenes)
        }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        for scene in scenes(content) {
            var bounds = scene.convert(scene.bounds, to: content)
            if content.isFlipped { bounds.origin.y = content.bounds.height - bounds.maxY }
            scene.snapshot().draw(in: bounds)
        }
        NSGraphicsContext.restoreGraphicsState()
        if let data = bitmap.representation(using: .png, properties: [:]) {
            try? data.write(to: URL(fileURLWithPath: path), options: .atomic)
        }
        #endif
    }
    static var spinning: Bool {
        #if DEBUG
        return ProcessInfo.processInfo.environment["MUSE_TEST_SNAPSHOT"] == nil
        #else
        return true
        #endif
    }
    static func reply(_ messages: [ChatMessage], failure: String?) {
        #if DEBUG
        guard let path = ProcessInfo.processInfo.environment["MUSE_TEST_REPLY"] else { return }
        let text = failure.map { "ERROR: " + $0 } ?? messages.filter { !$0.user }.map(\.text).joined(separator: "\n\n")
        try? text.write(toFile: path, atomically: true, encoding: .utf8)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { snapshot() }
        #endif
    }
}
