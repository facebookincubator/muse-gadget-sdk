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
 * Open sample videos for the clip buttons: short excerpts of the Blender
 * Foundation's open movies (CC BY 3.0), which the phone streams from the
 * Internet Archive when one is first played, decodes and re-encodes for the
 * gadget (ClipStreamer). None of them is in this repository or the app.
 * Their credits show while they play.
 */
struct Sample {
    let name, label, url: String
    let startS, seconds, fps: Int
    let credit: String
}

enum SampleClips {
    static let all = [
        Sample(name: "bunny", label: "Bunny",
               url: "https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4",
               startS: 104, seconds: 8, fps: 24,
               credit: "Big Buck Bunny, (c) copyright 2008 Blender Foundation, www.bigbuckbunny.org, CC BY 3.0"),
        Sample(name: "sintel", label: "Sintel", url: "https://archive.org/download/Sintel/sintel-2048-stereo.mp4",
               startS: 266, seconds: 8, fps: 24,
               credit: "Sintel, (c) copyright Blender Foundation, durian.blender.org, CC BY 3.0"),
    ]

    static func get(_ name: String) -> Sample? {
        all.first { $0.name == name }
    }
}
