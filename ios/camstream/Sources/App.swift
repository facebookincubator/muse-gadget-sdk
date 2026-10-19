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
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        true
    }

    func application(_ application: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let c = UISceneConfiguration(name: "Default", sessionRole: session.role)
        c.delegateClass = SceneDelegate.self
        return c
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private let main = MainViewController()

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = main
        w.makeKeyAndVisible()
        window = w
        UIApplication.shared.isIdleTimerDisabled = true
        for c in options.urlContexts {
            open(c.url)
        }
    }

    /* musegadgetstest://pick/<what> (or the old musegadgetscamera://) does what tapping
     * it does, as Android's --es pick
     * (xcrun devicectl device process launch --payload-url ...). */
    func scene(_ scene: UIScene, openURLContexts contexts: Set<UIOpenURLContext>) {
        for c in contexts {
            open(c.url)
        }
    }

    /* Debug builds only: any app or web page can open a URL, and a pick can
     * start streaming the camera. */
    private func open(_ url: URL) {
        #if DEBUG
        if url.host == "pick", url.pathComponents.count > 1 {
            let what = url.pathComponents[1]
            DispatchQueue.main.async { self.main.pick(what) }
        }
        #endif
    }
}
