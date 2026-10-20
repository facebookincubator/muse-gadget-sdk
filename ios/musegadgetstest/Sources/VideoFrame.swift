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
import Foundation

/*
 * The header before each access unit in a stream (protocols/VIDEO.md):
 * magic, flags, uint16 frame number, uint32 access unit length, little-endian.
 */
enum VideoFrame {
    static let magic: UInt8 = 0x56   /* 'V' */
    static let headerLen = 8
    static let keyframe: UInt8 = 1

    static func make(_ au: Data, number: Int, key: Bool) -> Data {
        var fr = Data(count: headerLen)
        let n = au.count
        fr[0] = magic
        fr[1] = key ? keyframe : 0
        fr[2] = UInt8(number & 0xff)
        fr[3] = UInt8((number >> 8) & 0xff)
        fr[4] = UInt8(n & 0xff)
        fr[5] = UInt8((n >> 8) & 0xff)
        fr[6] = UInt8((n >> 16) & 0xff)
        fr[7] = UInt8((n >> 24) & 0xff)
        fr.append(au)
        return fr
    }

    static func number(_ frame: Data) -> Int {
        let b = frame.startIndex
        return Int(frame[b + 2]) | Int(frame[b + 3]) << 8
    }
}
