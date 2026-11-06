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
 * The screen, as on Android (AppScreen.java): the app bar (its name, the
 * gadget it's connected to), the display (the preview of what's sent, the
 * stats over it, and what's playing with its Stop), the selected tab's
 * list, and the tab bar.
 */
final class AppScreen: UIView, UITabBarDelegate {
    let overlay = StatsOverlay()
    /* The camera's preview layer goes in previewHost; a clip's still (or the bars) in still. */
    let previewHost = UIView(), still = UIImageView()
    private weak var app: MainViewController?
    private(set) var tabs: [Tab] = []
    private let title = UILabel(), chip = UIButton(type: .system), chipDot = UIView()
    private let box = UIView(), placeholder = UILabel(), nowBar = UIView(), nowDot = UIView()
    private let nowTitle = UILabel(), nowSub = UILabel(), nowButton = UIButton(type: .system)
    private let scrim = CAGradientLayer()
    private let content = UIView(), tabBar = UITabBar()
    private var boxHeight: NSLayoutConstraint!
    private var nowAction: (() -> Void)?
    private var selected = -1

    init(app: MainViewController) {
        self.app = app
        super.init(frame: .zero)
        backgroundColor = Theme.bg
        tabs = Tabs.create(app)
        for v in [title, chip, box, content, tabBar] as [UIView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }

        /* App bar */
        title.text = "Muse Gadgets Test"
        title.font = .systemFont(ofSize: 22, weight: .semibold)
        title.textColor = Theme.text
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        var cc = UIButton.Configuration.filled()
        cc.baseBackgroundColor = Theme.surfaceHi
        cc.baseForegroundColor = Theme.text
        cc.cornerStyle = .capsule
        cc.contentInsets = NSDirectionalEdgeInsets(top: 6, leading: 26, bottom: 6, trailing: 14)
        cc.titleTextAttributesTransformer = UIConfigurationTextAttributesTransformer { a in
            var a = a
            a.font = .systemFont(ofSize: 14)
            return a
        }
        chip.configuration = cc
        chip.addAction(UIAction { [weak self] _ in self?.select(self?.tabIndex("Gadget") ?? 0) }, for: .touchUpInside)
        chipDot.layer.cornerRadius = 4
        chipDot.isUserInteractionEnabled = false
        chipDot.translatesAutoresizingMaskIntoConstraints = false
        chip.addSubview(chipDot)

        /* The display */
        box.backgroundColor = .black
        box.clipsToBounds = true
        still.contentMode = .scaleAspectFill
        still.clipsToBounds = true
        placeholder.textColor = Theme.text2
        placeholder.font = .systemFont(ofSize: 15)
        placeholder.textAlignment = .center
        placeholder.numberOfLines = 2
        for v in [previewHost, still, placeholder, overlay, nowBar] as [UIView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            box.addSubview(v)
        }
        box.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapDisplay)))
        scrim.colors = [UIColor.clear.cgColor, UIColor(white: 0, alpha: 0.6).cgColor, UIColor(white: 0, alpha: 0.9).cgColor]
        nowBar.layer.addSublayer(scrim)
        nowDot.layer.cornerRadius = 5
        nowTitle.font = .systemFont(ofSize: 16, weight: .semibold)
        nowTitle.textColor = Theme.text
        nowSub.font = .systemFont(ofSize: 12)
        nowSub.textColor = UIColor(white: 0.75, alpha: 1)
        var bc = UIButton.Configuration.filled()
        bc.cornerStyle = .capsule
        bc.contentInsets = NSDirectionalEdgeInsets(top: 8, leading: 18, bottom: 8, trailing: 18)
        nowButton.configuration = bc
        nowButton.addAction(UIAction { [weak self] _ in
            self?.nowAction?()
            self?.refresh()
        }, for: .touchUpInside)
        let texts = UIStackView(arrangedSubviews: [nowTitle, nowSub])
        texts.axis = .vertical
        texts.spacing = 2
        let nowRow = UIStackView(arrangedSubviews: [nowDot, texts, nowButton])
        nowRow.alignment = .center
        nowRow.spacing = 10
        nowRow.translatesAutoresizingMaskIntoConstraints = false
        nowBar.addSubview(nowRow)
        nowButton.setContentCompressionResistancePriority(.required, for: .horizontal)
        nowButton.setContentHuggingPriority(.required, for: .horizontal)
        texts.setContentHuggingPriority(.defaultLow, for: .horizontal)
        texts.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        /* The tabs' lists and the tab bar */
        var items: [UITabBarItem] = []
        for (i, t) in tabs.enumerated() {
            t.list.host = app
            for s in t.sections {
                s(app, t.list)
            }
            t.list.table.translatesAutoresizingMaskIntoConstraints = false
            t.list.table.isHidden = true
            content.addSubview(t.list.table)
            NSLayoutConstraint.activate([
                t.list.table.leadingAnchor.constraint(equalTo: content.leadingAnchor),
                t.list.table.trailingAnchor.constraint(equalTo: content.trailingAnchor),
                t.list.table.topAnchor.constraint(equalTo: content.topAnchor),
                t.list.table.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            ])
            items.append(UITabBarItem(title: t.title, image: UIImage(systemName: t.symbol), tag: i))
        }
        tabBar.items = items
        tabBar.delegate = self
        let ap = UITabBarAppearance()
        ap.configureWithOpaqueBackground()
        ap.backgroundColor = Theme.nav
        for l in [ap.stackedLayoutAppearance] {
            l.selected.iconColor = Theme.accent
            l.selected.titleTextAttributes = [.foregroundColor: Theme.text]
            l.normal.iconColor = Theme.text2
            l.normal.titleTextAttributes = [.foregroundColor: Theme.text2]
            l.normal.badgeBackgroundColor = Theme.good
        }
        tabBar.standardAppearance = ap
        tabBar.scrollEdgeAppearance = ap

        let g = safeAreaLayoutGuide
        let side = min(UIScreen.main.bounds.width, UIScreen.main.bounds.height * 0.36)
        boxHeight = box.heightAnchor.constraint(equalToConstant: side)
        NSLayoutConstraint.activate([
            title.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 20),
            title.topAnchor.constraint(equalTo: g.topAnchor, constant: 6),
            chip.leadingAnchor.constraint(greaterThanOrEqualTo: title.trailingAnchor, constant: 8),
            chip.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -14),
            chip.centerYAnchor.constraint(equalTo: title.centerYAnchor),
            chipDot.widthAnchor.constraint(equalToConstant: 8),
            chipDot.heightAnchor.constraint(equalToConstant: 8),
            chipDot.leadingAnchor.constraint(equalTo: chip.leadingAnchor, constant: 12),
            chipDot.centerYAnchor.constraint(equalTo: chip.centerYAnchor),
            box.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 10),
            box.leadingAnchor.constraint(equalTo: leadingAnchor),
            box.trailingAnchor.constraint(equalTo: trailingAnchor),
            boxHeight,
            previewHost.topAnchor.constraint(equalTo: box.topAnchor),
            previewHost.centerXAnchor.constraint(equalTo: box.centerXAnchor),
            previewHost.widthAnchor.constraint(equalToConstant: side),
            previewHost.heightAnchor.constraint(equalToConstant: side),
            still.topAnchor.constraint(equalTo: previewHost.topAnchor),
            still.leadingAnchor.constraint(equalTo: previewHost.leadingAnchor),
            still.trailingAnchor.constraint(equalTo: previewHost.trailingAnchor),
            still.bottomAnchor.constraint(equalTo: previewHost.bottomAnchor),
            placeholder.centerXAnchor.constraint(equalTo: box.centerXAnchor),
            placeholder.bottomAnchor.constraint(equalTo: box.bottomAnchor, constant: -84),
            overlay.topAnchor.constraint(equalTo: box.topAnchor),
            overlay.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            overlay.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            overlay.bottomAnchor.constraint(equalTo: box.bottomAnchor),
            nowBar.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            nowBar.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            nowBar.bottomAnchor.constraint(equalTo: box.bottomAnchor),
            nowRow.leadingAnchor.constraint(equalTo: nowBar.leadingAnchor, constant: 16),
            nowRow.trailingAnchor.constraint(equalTo: nowBar.trailingAnchor, constant: -12),
            nowRow.topAnchor.constraint(equalTo: nowBar.topAnchor, constant: 24),
            nowRow.bottomAnchor.constraint(equalTo: nowBar.bottomAnchor, constant: -10),
            nowDot.widthAnchor.constraint(equalToConstant: 10),
            nowDot.heightAnchor.constraint(equalToConstant: 10),
            content.topAnchor.constraint(equalTo: box.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: leadingAnchor),
            content.trailingAnchor.constraint(equalTo: trailingAnchor),
            content.bottomAnchor.constraint(equalTo: tabBar.topAnchor),
            tabBar.leadingAnchor.constraint(equalTo: leadingAnchor),
            tabBar.trailingAnchor.constraint(equalTo: trailingAnchor),
            tabBar.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        self.side = side
        select(0)
    }

    private var side: CGFloat = 0

    required init?(coder: NSCoder) {
        fatalError()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        scrim.frame = nowBar.bounds
        previewHost.layer.sublayers?.forEach { $0.frame = previewHost.bounds }
    }

    @objc private func tapDisplay() {
        app?.setOverlayMode(overlay.mode == .compact ? .detailed : .compact)
    }

    func tabIndex(_ title: String) -> Int {
        tabs.firstIndex { $0.title == title } ?? 0
    }

    func tabBar(_ tabBar: UITabBar, didSelect item: UITabBarItem) {
        select(item.tag)
    }

    func select(_ k: Int) {
        if k == selected {
            tabs[k].list.table.setContentOffset(.zero, animated: true)
            return
        }
        selected = k
        for (i, t) in tabs.enumerated() {
            t.list.table.isHidden = i != k
        }
        tabBar.selectedItem = tabBar.items?[k]
        refresh()
    }

    /* Scrolls the tab to the section titled header (a "pick rate", say). */
    func show(_ tab: String, _ header: String) {
        select(tabIndex(tab))
        let t = tabs[selected].list.table
        for s in 0..<t.numberOfSections where t.dataSource?.tableView?(t, titleForHeaderInSection: s) == header {
            if t.numberOfRows(inSection: s) > 0 {
                t.scrollToRow(at: IndexPath(row: 0, section: s), at: .top, animated: true)
            }
        }
    }

    private func setButton(_ label: String, _ fg: UIColor, _ bg: UIColor, _ act: @escaping () -> Void) {
        if nowButton.configuration?.title != label {
            nowButton.configuration?.title = label
            nowButton.configuration?.baseForegroundColor = fg
            nowButton.configuration?.baseBackgroundColor = bg
        }
        nowAction = act
    }

    /* Brings everything on screen up to date. */
    func refresh() {
        guard let app else { return }
        let connected = app.connected()
        let name = app.gadgetName()
        let short = name.map { $0.hasPrefix("MuseGadget-") ? String($0.dropFirst(11)) : $0 }
        chipDot.backgroundColor = connected ? Theme.good : Theme.warn
        chip.configuration?.title = connected ? short : name != nil ? "Connecting…" : "No gadget"

        let playing = app.streamingOrPreparing()
        if !connected {
            nowDot.backgroundColor = Theme.text3
            nowTitle.text = name != nil ? "Connecting to \(name!)" : "Looking for a gadget"
            nowSub.text = app.connectionLine()
            setButton("Choose", Theme.text, Theme.surfaceHi) { [weak app] in app?.pick("device") }
        } else if playing {
            nowDot.backgroundColor = app.preparing() ? Theme.warn : Theme.good
            nowTitle.text = app.sourceLabel() + (app.preparing() ? " · preparing" : "")
            nowSub.text = "Video tab → " + (name ?? "")
            setButton("Stop", UIColor(red: 0.1, green: 0.05, blue: 0.05, alpha: 1), Theme.bad) { [weak app] in app?.play("stop") }
        } else {
            nowDot.backgroundColor = Theme.accent
            nowTitle.text = "Ready: " + app.sourceLabel()
            nowSub.text = "Tap Start, or a source below"
            setButton("Start", UIColor(red: 0.04, green: 0.1, blue: 0.2, alpha: 1), Theme.accent) { [weak app] in
                guard let app else { return }
                app.play(app.currentSource())
            }
        }
        /* The camera shows itself; a clip shows its still, the bars a test card. */
        let camera = playing && app.sourceIs("camera")
        previewHost.isHidden = !camera
        var img: UIImage?
        if playing && app.sourceIs("pattern") {
            img = Thumbs.bars
        } else if playing, let n = app.clipName() {
            img = app.clipStill(n)
        }
        still.image = img
        still.isHidden = img == nil
        placeholder.isHidden = camera || img != nil
        placeholder.text = !connected ? "No gadget yet" : playing ? (app.preparing() ? "Preparing the clip…" : "Sending " + app.sourceLabel())
            : "Not streaming"
        overlay.isHidden = overlay.mode == .off || !playing
        let h = max(side, overlay.mode == .detailed ? overlay.neededHeight + nowBar.bounds.height - 24 : 0)
        if boxHeight.constant != h {
            boxHeight.constant = h
        }
        for (i, t) in tabs.enumerated() {
            tabBar.items?[i].badgeValue = t.live() ? "" : nil
        }
        if selected >= 0 {
            tabs[selected].list.refresh()
        }
    }
}
