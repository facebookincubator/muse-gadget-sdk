// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
// Procedural character artwork follows the Jollybot terms described in README.
import AppKit

let destination = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
func icon(_ size: Int) throws -> Data {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let transform = AffineTransform(scale: CGFloat(size) / 1024)
    (transform as NSAffineTransform).concat()
    func ellipse(_ rect: NSRect, _ color: NSColor) { color.setFill(); NSBezierPath(ovalIn: rect).fill() }
    let background = NSBezierPath(roundedRect: NSRect(x: 26, y: 26, width: 972, height: 972), xRadius: 218, yRadius: 218)
    NSGradient(starting: NSColor(calibratedRed: 0.29, green: 0.20, blue: 0.48, alpha: 1),
               ending: NSColor(calibratedRed: 0.09, green: 0.065, blue: 0.16, alpha: 1))!.draw(in: background, angle: -70)
    background.addClip()
    let original = NSImage(contentsOfFile: "Sources/MuseCompanion/Resources/jollybot.gif")!
    original.draw(in: NSRect(x: 26, y: 26, width: 972, height: 972))
    NSGraphicsContext.restoreGraphicsState()
    return bitmap.representation(using: .png, properties: [:])!
}
for pointSize in [16, 32, 128, 256, 512] {
    try icon(pointSize).write(to: destination.appendingPathComponent("icon_\(pointSize)x\(pointSize).png"))
    try icon(pointSize * 2).write(to: destination.appendingPathComponent("icon_\(pointSize)x\(pointSize)@2x.png"))
}
