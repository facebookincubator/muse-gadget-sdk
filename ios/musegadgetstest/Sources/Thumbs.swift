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
import AVFoundation
import UIKit

/*
 * Thumbnails for the clip rows, and the clip's still on the preview while
 * it plays: one frame of each clip, taken in the background the first time
 * it's asked for and kept in the app's caches (a sample clip's costs a few
 * range requests, once). get() returns nil until it's ready.
 */
enum Thumbs {
    private static var done: [String: UIImage] = [:]
    private static var asked: Set<String> = []
    private static let queue = DispatchQueue(label: "thumbs", qos: .utility)

    static func cached(_ key: String) -> UIImage? {
        done[key]
    }

    static func get(_ key: String, at seconds: Double, asset: @escaping () -> AVAsset?) -> UIImage? {
        if let d = done[key] {
            return d
        }
        if asked.contains(key) {
            return nil
        }
        asked.insert(key)
        let file = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("thumb-\(key).jpg")
        queue.async {
            var img = UIImage(contentsOfFile: file.path)
            if img == nil, let a = asset() {
                let g = AVAssetImageGenerator(asset: a)
                g.appliesPreferredTrackTransform = true
                g.maximumSize = CGSize(width: 480, height: 480)
                let sem = DispatchSemaphore(value: 0)
                g.generateCGImageAsynchronously(for: CMTime(seconds: seconds, preferredTimescale: 600)) { cg, _, err in
                    if let cg {
                        img = UIImage(cgImage: cg)
                        try? img?.jpegData(compressionQuality: 0.85)?.write(to: file)
                    } else if let err {
                        mlog("thumbnail \(key): \(err.localizedDescription)")
                    }
                    sem.signal()
                }
                sem.wait()
            }
            if let img {
                DispatchQueue.main.async { done[key] = img }
            }
        }
        return nil
    }

    /* Color bars, for the source row and the preview of the bars. */
    static let bars: UIImage = {
        let colors: [UIColor] = [UIColor(white: 0.75, alpha: 1), UIColor(red: 0.75, green: 0.75, blue: 0, alpha: 1),
                                 UIColor(red: 0, green: 0.75, blue: 0.75, alpha: 1), UIColor(red: 0, green: 0.75, blue: 0, alpha: 1),
                                 UIColor(red: 0.75, green: 0, blue: 0.75, alpha: 1), UIColor(red: 0.75, green: 0, blue: 0, alpha: 1),
                                 UIColor(red: 0, green: 0, blue: 0.75, alpha: 1)]
        return UIGraphicsImageRenderer(size: CGSize(width: 140, height: 80)).image { ctx in
            let w = 140.0 / Double(colors.count)
            for (i, c) in colors.enumerated() {
                c.setFill()
                ctx.fill(CGRect(x: Double(i) * w, y: 0, width: w + 1, height: 80))
            }
        }
    }()
}
