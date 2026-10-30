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
import SwiftUI

struct ContentView: View {
    @ObservedObject var model: GadgetModel
    @State private var message = ""

    var body: some View {
        NavigationStack {
            Form {
                Section("This gadget") {
                    LabeledContent("BLE name", value: model.identity.bleName)
                    LabeledContent("Node id", value: model.identity.nodeID)
                    LabeledContent("Paired", value: model.paired ? "yes" : "no")
                }
                Section("SDK token (gadgets.muse.ai)") {
                    SecureField("mgst_…", text: $model.sdkToken)
                    Button("Save token", action: model.saveSDKToken)
                }
                Section("Pairing") {
                    if model.pairingOpen {
                        Text("Advertising. Keep this app open while the Muse app on another phone adds the device.")
                            .font(.footnote)
                        Button("Stop", role: .cancel, action: model.stopPairing)
                    } else {
                        Button(model.paired ? "Pair again" : "Open pairing", action: model.startPairing)
                    }
                    if model.paired {
                        Button("Forget pairing", role: .destructive, action: model.unpair)
                    }
                }
                Section("Message your Muse") {
                    TextField("Message", text: $message)
                    Button("Send") {
                        model.sendToMuse(message)
                        message = ""
                    }
                    .disabled(message.isEmpty)
                }
                Section("Log") {
                    ForEach(Array(model.logLines.suffix(80).enumerated()), id: \.offset) { _, line in
                        Text(line).font(.caption.monospaced())
                    }
                }
            }
            .navigationTitle("Muse Gadget")
        }
        .fullScreenCover(item: Binding(get: { model.shownText.map(ShownText.init) },
                                       set: { model.shownText = $0?.text })) { shown in
            Text(shown.text)
                .font(.system(size: 44, weight: .bold))
                .multilineTextAlignment(.center)
                .padding()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .onTapGesture { model.shownText = nil }
        }
    }
}

private struct ShownText: Identifiable {
    let text: String
    var id: String { text }
}
