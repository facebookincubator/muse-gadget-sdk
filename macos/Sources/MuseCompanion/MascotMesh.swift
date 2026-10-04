// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
// The original Jollybot texture retains its separate artwork terms.
import AppKit
import SceneKit

enum MascotMesh {
    /// Extrude the source sprite's silhouette into a closed pixel mesh. The
    /// front uses the unmodified original artwork, rather than a redrawn face.
    static func make() -> SCNNode {
        let resources = Bundle.main.url(forResource: "MuseCompanion_MuseCompanion", withExtension: "bundle").flatMap(Bundle.init(url:)) ?? Bundle.module
        guard let url = resources.url(forResource: "jollybot", withExtension: "gif"),
              let image = NSImage(contentsOf: url),
              let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return SCNNode() }
        let width = cg.width, height = cg.height, step = 4
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { buffer in
            let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8,
                bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.draw(cg, in: CGRect(x: 0, y: 0, width: width, height: height))
        }
        let columns = width / step, rows = height / step
        var left = [Int](repeating: columns, count: rows), right = [Int](repeating: -1, count: rows)
        // Beige/brown pixels determine row spans; enclosed black eyes and mouth
        // stay inside the silhouette. Purple background stars are excluded.
        for row in 0..<rows {
            for column in 0..<columns {
                let offset = ((row * step + step / 2) * width + column * step + step / 2) * 4
                let r = Int(pixels[offset]), g = Int(pixels[offset + 1]), b = Int(pixels[offset + 2])
                if r > 55 && g > 40 && r >= g && g > b {
                    left[row] = min(left[row], column); right[row] = max(right[row], column)
                }
            }
        }
        func occupied(_ x: Int, _ y: Int) -> Bool {
            y >= 0 && y < rows && x >= left[y] && x <= right[y]
        }
        var vertices: [SCNVector3] = [], normals: [SCNVector3] = [], coordinates: [CGPoint] = []
        var front: [Int32] = [], shell: [Int32] = []
        let firstRow = left.firstIndex(where: { $0 < columns }) ?? 0
        let lastRow = right.lastIndex(where: { $0 >= 0 }) ?? rows - 1
        let centerY = Float(firstRow + lastRow + 1) / 2
        let radiusY = max(1, Float(lastRow - firstRow + 1) / 2)
        let centerX = Float(columns) / 2
        let radiusX = max(1, Float((right.max() ?? columns) - (left.min() ?? 0) + 1) / 2)
        func point(_ x: Int, _ y: Int, _ back: Bool, _ row: Int) -> SCNVector3 {
            let nx = (Float(x) - centerX) / radiusX
            let ny = (Float(y) - centerY) / radiusY
            let depth = 0.12 + 0.83 * sqrt(max(0, 1 - nx * nx - ny * ny * 0.74))
            return SCNVector3((Float(x * step) / Float(width) - 0.5) * 3.7 * 1.18,
                             (0.5 - Float(y * step) / Float(height)) * 3.7 * 0.97,
                             back ? -depth * 0.75 : depth * 0.38)
        }
        func quad(_ points: [SCNVector3], _ uvs: [CGPoint], _ normal: SCNVector3, _ texture: Bool) {
            let offset = Int32(vertices.count)
            vertices.append(contentsOf: points); coordinates.append(contentsOf: uvs)
            normals.append(contentsOf: Array(repeating: normal, count: 4))
            let indices = [offset, offset + 3, offset + 2, offset, offset + 2, offset + 1]
            if texture { front.append(contentsOf: indices) } else { shell.append(contentsOf: indices) }
        }
        for y in 0..<rows where right[y] >= left[y] {
            for x in left[y]...right[y] {
                let xy = [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]
                let f = xy.map { point($0.0, $0.1, false, y) }
                let b = xy.map { point($0.0, $0.1, true, y) }
                let uv = xy.map { CGPoint(x: Double($0.0) / Double(columns), y: Double($0.1) / Double(rows)) }
                quad(f, uv, SCNVector3(0, 0, 1), true)
                quad(b.reversed(), uv.reversed(), SCNVector3(0, 0, -1), false)
                for (edge, neighbor, normal) in [
                    (0, (x, y - 1), SCNVector3(0, 1, 0)),
                    (1, (x + 1, y), SCNVector3(1, 0, 0)),
                    (2, (x, y + 1), SCNVector3(0, -1, 0)),
                    (3, (x - 1, y), SCNVector3(-1, 0, 0))
                ] where !occupied(neighbor.0, neighbor.1) {
                    let next = (edge + 1) % 4
                    quad([f[next], f[edge], b[edge], b[next]], Array(repeating: .zero, count: 4), normal, false)
                }
            }
        }
        let geometry = SCNGeometry(sources: [SCNGeometrySource(vertices: vertices), SCNGeometrySource(normals: normals),
                                            SCNGeometrySource(textureCoordinates: coordinates)], elements: [
            SCNGeometryElement(indices: front, primitiveType: .triangles),
            SCNGeometryElement(indices: shell, primitiveType: .triangles)])
        let artwork = SCNMaterial(); artwork.diffuse.contents = cg; artwork.lightingModel = .constant
        artwork.diffuse.magnificationFilter = .nearest; artwork.diffuse.minificationFilter = .nearest
        artwork.isDoubleSided = true
        let back = SCNMaterial(); back.diffuse.contents = NSColor(calibratedRed: 0.66, green: 0.59, blue: 0.46, alpha: 1)
        back.lightingModel = .physicallyBased; back.roughness.contents = 1; back.isDoubleSided = true
        geometry.materials = [artwork, back]
        return SCNNode(geometry: geometry)
    }
}
