import AppKit
let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 1024, pixelsHigh: 1024, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
let ctx = NSGraphicsContext.current!.cgContext
ctx.setFillColor(NSColor(calibratedRed: 0.025, green: 0.12, blue: 0.15, alpha: 1).cgColor)
ctx.fill(CGRect(x: 0, y: 0, width: 1024, height: 1024))
for i in 0..<4 {
    ctx.setStrokeColor(NSColor(calibratedRed: 0.5, green: 0.9, blue: 0.85, alpha: 0.65 - Double(i) * 0.12).cgColor)
    ctx.setLineWidth(8)
    let w = CGFloat(220 + i * 175), h = CGFloat(80 + i * 70)
    ctx.strokeEllipse(in: CGRect(x: 512-w/2, y: 340-h/2, width: w, height: h))
}
let path = NSBezierPath()
path.move(to: CGPoint(x: 512, y: 800))
path.curve(to: CGPoint(x: 370, y: 520), controlPoint1: CGPoint(x: 480, y: 690), controlPoint2: CGPoint(x: 370, y: 620))
path.curve(to: CGPoint(x: 654, y: 520), controlPoint1: CGPoint(x: 370, y: 340), controlPoint2: CGPoint(x: 654, y: 340))
path.curve(to: CGPoint(x: 512, y: 800), controlPoint1: CGPoint(x: 654, y: 620), controlPoint2: CGPoint(x: 550, y: 690))
NSColor(calibratedRed: 0.65, green: 0.96, blue: 0.91, alpha: 1).setFill(); path.fill()
NSGraphicsContext.restoreGraphicsState()
try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "Watch/Assets.xcassets/AppIcon.appiconset/AppIcon.png"))
