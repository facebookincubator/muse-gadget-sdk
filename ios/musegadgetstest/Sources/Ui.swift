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

/*
 * The app's look (a dark palette) and its settings-style list: sections of
 * rows in an inset grouped table, each row a title, a line of description
 * and its current value, a switch, a picker or a slider. Rows read their
 * text from closures, so refresh() brings the list up to date (once a
 * second, and after every change). The same model as Android's Ui.java.
 */
enum Theme {
    static let bg = UIColor(red: 0.055, green: 0.063, blue: 0.075, alpha: 1)
    static let surface = UIColor(red: 0.102, green: 0.114, blue: 0.133, alpha: 1)
    static let surfaceHi = UIColor(red: 0.141, green: 0.157, blue: 0.188, alpha: 1)
    static let nav = UIColor(red: 0.082, green: 0.094, blue: 0.110, alpha: 1)
    static let text = UIColor(red: 0.91, green: 0.918, blue: 0.929, alpha: 1)
    static let text2 = UIColor(red: 0.604, green: 0.627, blue: 0.659, alpha: 1)
    static let text3 = UIColor(red: 0.4, green: 0.424, blue: 0.459, alpha: 1)
    static let accent = UIColor(red: 0.541, green: 0.706, blue: 0.973, alpha: 1)
    static let good = UIColor(red: 0.357, green: 0.839, blue: 0.541, alpha: 1)
    static let warn = UIColor(red: 1, green: 0.706, blue: 0.329, alpha: 1)
    static let bad = UIColor(red: 1, green: 0.42, blue: 0.42, alpha: 1)
    static let sending = UIColor(red: 1, green: 0.835, blue: 0.31, alpha: 1)
}

/* One row of a list; its closures are read on refresh(). */
final class Row {
    enum Kind {
        case value, toggle, slider, info
    }

    let kind: Kind
    var title: () -> String
    var detail: (() -> String)?
    var value: (() -> String)?
    var image: (() -> UIImage?)?
    var bigImage = false
    var badge: (() -> String?)?   /* "!TEXT": in the warning color */
    var isOn: (() -> Bool)?
    var setOn: ((Bool) -> Void)?
    var sliderValue: (() -> Float)?
    var setSlider: ((Float) -> Void)?
    var action: (() -> Void)?
    var visible: () -> Bool = { true }
    var enabled: () -> Bool = { true }

    init(_ kind: Kind, _ title: @escaping () -> String) {
        self.kind = kind
        self.title = title
    }

    @discardableResult func showIf(_ f: @escaping () -> Bool) -> Row {
        visible = f
        return self
    }

    @discardableResult func enabledIf(_ f: @escaping () -> Bool) -> Row {
        enabled = f
        return self
    }

    @discardableResult func withBadge(_ f: @escaping () -> String?) -> Row {
        badge = f
        return self
    }
}

/* A scrolling list of sections: header() starts one, the row builders add to it. */
final class SettingsList: NSObject, UITableViewDataSource, UITableViewDelegate {
    private struct Section {
        let title: String
        var rows: [Row] = []
        var footer: String?
    }

    let table = UITableView(frame: .zero, style: .insetGrouped)
    private var sections: [Section] = []
    private var shown: [[Row]] = []
    weak var host: UIViewController?
    var needsRelayout = false

    override init() {
        super.init()
        table.backgroundColor = Theme.bg
        table.dataSource = self
        table.delegate = self
        table.separatorColor = UIColor(white: 1, alpha: 0.08)
        table.sectionHeaderTopPadding = 6
    }

    @discardableResult func header(_ title: String) -> SettingsList {
        sections.append(Section(title: title))
        return self
    }

    /* Small print under the current section. */
    @discardableResult func footnote(_ s: String) -> SettingsList {
        sections[sections.count - 1].footer = s
        return self
    }

    private func add(_ r: Row) -> Row {
        if sections.isEmpty {
            header("")
        }
        sections[sections.count - 1].rows.append(r)
        return r
    }

    /* A row with a value on the right; action may be nil (information only). */
    @discardableResult func row(_ title: @escaping () -> String, detail: (() -> String)? = nil, value: (() -> String)? = nil,
                                image: (() -> UIImage?)? = nil, bigImage: Bool = false,
                                action: (() -> Void)? = nil) -> Row {
        let r = Row(.value, title)
        r.detail = detail
        r.value = value
        r.image = image
        r.bigImage = bigImage
        r.action = action
        return add(r)
    }

    @discardableResult func toggle(_ title: String, detail: @escaping () -> String, isOn: @escaping () -> Bool,
                                   set: @escaping (Bool) -> Void) -> Row {
        let r = Row(.toggle, { title })
        r.detail = detail
        r.isOn = isOn
        r.setOn = set
        return add(r)
    }

    /* A row whose value is one of labels; a tap opens a picker. */
    @discardableResult func choice(_ title: String, detail: @escaping () -> String, labels: @escaping () -> [String],
                                   selected: @escaping () -> Int, pick: @escaping (Int) -> Void) -> Row {
        let r = Row(.value, { title })
        r.detail = detail
        r.value = {
            let l = labels(), i = selected()
            return i >= 0 && i < l.count ? l[i] : ""
        }
        r.action = { [weak self] in
            self?.choose(title, labels(), selected(), pick)
        }
        return add(r)
    }

    /* A slider under the title; set gets the value when the finger lifts. */
    @discardableResult func slider(_ title: String, value: @escaping () -> String, get: @escaping () -> Float,
                                   set: @escaping (Float) -> Void) -> Row {
        let r = Row(.slider, { title })
        r.value = value
        r.sliderValue = get
        r.setSlider = set
        return add(r)
    }

    /* A compact label / value line (the gadget's report, stats). */
    @discardableResult func info(_ label: String, _ value: @escaping () -> String) -> Row {
        let r = Row(.info, { label })
        r.value = value
        return add(r)
    }

    func choose(_ title: String, _ labels: [String], _ selected: Int, _ pick: @escaping (Int) -> Void) {
        let a = UIAlertController(title: title, message: nil, preferredStyle: .actionSheet)
        for (i, l) in labels.enumerated() {
            a.addAction(UIAlertAction(title: (i == selected ? "\u{2713} " : "") + l, style: .default) { [weak self] _ in
                pick(i)
                self?.refresh()
            })
        }
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        a.popoverPresentationController?.sourceView = table
        host?.present(a, animated: true)
    }

    /* Rows shown change (showIf): reload; otherwise update the visible cells in place. */
    func refresh() {
        let now = sections.map { $0.rows.filter { $0.visible() } }
        if now.map({ $0.map { ObjectIdentifier($0) } }) != shown.map({ $0.map { ObjectIdentifier($0) } }) {
            shown = now
            table.reloadData()
            return
        }
        for ip in table.indexPathsForVisibleRows ?? [] {
            if let c = table.cellForRow(at: ip) as? RowCell, ip.section < shown.count, ip.row < shown[ip.section].count {
                c.update(shown[ip.section][ip.row])
            }
        }
        if needsRelayout {
            needsRelayout = false
            table.performBatchUpdates(nil)
        }
    }

    func numberOfSections(in tableView: UITableView) -> Int {
        if shown.isEmpty {
            shown = sections.map { $0.rows.filter { $0.visible() } }
        }
        return sections.count
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        shown[section].count
    }

    func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        sections[section].title
    }

    func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        sections[section].footer
    }

    func tableView(_ tableView: UITableView, willDisplayHeaderView view: UIView, forSection section: Int) {
        if let h = view as? UITableViewHeaderFooterView {
            var c = h.defaultContentConfiguration()
            c.text = sections[section].title
            c.textProperties.color = Theme.accent
            c.textProperties.font = .systemFont(ofSize: 14, weight: .semibold)
            h.contentConfiguration = c
        }
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let c = RowCell()
        c.list = self
        c.update(shown[indexPath.section][indexPath.row])
        return c
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let r = shown[indexPath.section][indexPath.row]
        guard r.enabled() else { return }
        if r.kind == .toggle, let on = r.isOn {
            r.setOn?(!on())
        } else {
            r.action?()
        }
        refresh()
    }
}

/* One cell, laid out for its row's kind. */
final class RowCell: UITableViewCell {
    weak var list: SettingsList?
    private weak var row: Row?
    private let title = UILabel(), detail = UILabel(), value = UILabel(), badge = PaddedLabel()
    private let thumb = UIImageView()
    private let toggle = UISwitch(), slider = UISlider()
    private var built = false

    private func build(_ r: Row) {
        built = true
        backgroundColor = Theme.surface
        selectedBackgroundView = UIView()
        selectedBackgroundView?.backgroundColor = Theme.surfaceHi
        title.font = .systemFont(ofSize: 17)
        title.textColor = r.kind == .info ? Theme.text2 : Theme.text
        detail.font = .systemFont(ofSize: 14)
        detail.textColor = Theme.text2
        detail.numberOfLines = 3
        value.font = r.kind == .info ? .monospacedSystemFont(ofSize: 15, weight: .regular) : .systemFont(ofSize: 16)
        value.textColor = r.kind == .info ? Theme.text : Theme.accent
        value.textAlignment = .right
        value.setContentCompressionResistancePriority(.required, for: .horizontal)
        value.setContentHuggingPriority(.required, for: .horizontal)
        badge.font = .systemFont(ofSize: 11, weight: .bold)
        badge.textColor = Theme.bg
        badge.layer.cornerRadius = 10
        badge.layer.masksToBounds = true
        badge.setContentCompressionResistancePriority(.required, for: .horizontal)
        badge.setContentHuggingPriority(.required, for: .horizontal)
        thumb.contentMode = .scaleAspectFill
        thumb.clipsToBounds = true
        thumb.layer.cornerRadius = 10
        thumb.backgroundColor = Theme.surfaceHi
        thumb.tintColor = Theme.accent
        toggle.onTintColor = Theme.accent
        toggle.addAction(UIAction { [weak self] _ in
            guard let self, let r = self.row else { return }
            r.setOn?(self.toggle.isOn)
            self.list?.refresh()
        }, for: .valueChanged)
        slider.minimumValue = 0
        slider.maximumValue = 100
        slider.minimumTrackTintColor = Theme.accent
        slider.addAction(UIAction { [weak self] _ in
            guard let self, let r = self.row else { return }
            r.setSlider?(self.slider.value.rounded())
            self.list?.refresh()
        }, for: [.touchUpInside, .touchUpOutside])

        let texts = UIStackView(arrangedSubviews: [title, detail])
        texts.axis = .vertical
        texts.spacing = 3
        texts.setContentHuggingPriority(.defaultLow, for: .horizontal)
        texts.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        var items: [UIView] = []
        if r.image != nil {
            thumb.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                thumb.widthAnchor.constraint(equalToConstant: r.bigImage ? 64 : 40),
                thumb.heightAnchor.constraint(equalToConstant: 40),
            ])
            items.append(thumb)
        }
        items.append(texts)
        switch r.kind {
        case .toggle: items.append(toggle)
        case .value, .info: items.append(value); items.append(badge)
        case .slider: items.append(value)
        }
        let h = UIStackView(arrangedSubviews: items)
        h.axis = .horizontal
        h.alignment = .center
        h.spacing = 14
        let outer: UIStackView
        if r.kind == .slider {
            outer = UIStackView(arrangedSubviews: [h, slider])
            outer.axis = .vertical
            outer.spacing = 8
        } else {
            outer = UIStackView(arrangedSubviews: [h])
        }
        outer.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(outer)
        let m: CGFloat = r.kind == .info ? 12 : 14
        NSLayoutConstraint.activate([
            outer.leadingAnchor.constraint(equalTo: contentView.layoutMarginsGuide.leadingAnchor),
            outer.trailingAnchor.constraint(equalTo: contentView.layoutMarginsGuide.trailingAnchor),
            outer.topAnchor.constraint(equalTo: contentView.topAnchor, constant: m),
            outer.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -m),
        ])
    }

    func update(_ r: Row) {
        row = r
        if !built {
            build(r)
        }
        let en = r.enabled()
        contentView.alpha = en ? 1 : 0.45
        selectionStyle = r.action != nil || r.kind == .toggle ? .default : .none
        accessoryType = .none
        title.text = r.title()
        let d = r.detail?() ?? ""
        if built && detail.text != nil && detail.text != d && (detail.text ?? "").contains("\n") != d.contains("\n") {
            list?.needsRelayout = true   /* a line more or less: the row's height changes */
        }
        detail.text = d
        detail.isHidden = d.isEmpty
        value.text = r.value?() ?? ""
        value.isHidden = value.text?.isEmpty ?? true
        if let b = r.badge?() {
            let warn = b.hasPrefix("!")
            badge.text = warn ? String(b.dropFirst()) : b
            badge.backgroundColor = warn ? Theme.warn : Theme.good
            badge.isHidden = false
        } else {
            badge.isHidden = true
        }
        if let f = r.image {
            thumb.image = f()
        }
        if let f = r.isOn {
            toggle.setOn(f(), animated: true)
            toggle.isEnabled = en
        }
        if let f = r.sliderValue, !slider.isTracking {
            slider.value = f()
            slider.isEnabled = en
        }
    }
}

/* A label with padding, for badges. */
final class PaddedLabel: UILabel {
    override func drawText(in rect: CGRect) {
        super.drawText(in: rect.insetBy(dx: 8, dy: 3))
    }

    override var intrinsicContentSize: CGSize {
        let s = super.intrinsicContentSize
        return CGSize(width: s.width + 16, height: s.height + 6)
    }
}
