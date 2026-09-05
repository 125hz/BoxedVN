import SwiftUI
import UIKit

/// Guest keys follow the touch lifetime. Actions such as opening the keyboard
/// fire on touch-down, while key-up always runs on release or cancellation.
struct GuestPressButton: UIViewRepresentable {
    var glyph: String?
    var label: String
    var active: Bool
    var tint: UIColor = .systemBlue
    var onPress: () -> Void
    var onRelease: () -> Void = {}

    func makeUIView(context: Context) -> GuestPressControl { GuestPressControl(frame: .zero) }
    func updateUIView(_ view: GuestPressControl, context: Context) {
        view.onPress = onPress
        view.onRelease = onRelease
        view.isEnabled = active
        if !active { view.releasePress() }
        view.tintColor = tint
        view.alpha = active ? 1 : 0.4
        view.accessibilityLabel = label
        view.setTitle(glyph == nil ? label : nil, for: .normal)
        view.setTitleColor(tint, for: .normal)
        view.setImage(glyph.flatMap { UIImage(systemName: $0,
            withConfiguration: UIImage.SymbolConfiguration(pointSize: 20)) }, for: .normal)
    }
    static func dismantleUIView(_ view: GuestPressControl, coordinator: ()) {
        view.releasePress()
    }
}

final class GuestPressControl: UIButton {
    var onPress: () -> Void = {}
    var onRelease: () -> Void = {}
    private var guestPressActive = false
    override init(frame: CGRect) {
        super.init(frame: frame)
        titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
        NotificationCenter.default.addObserver(self, selector: #selector(releasePress),
            name: UIApplication.willResignActiveNotification, object: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    deinit { NotificationCenter.default.removeObserver(self) }
    override func beginTracking(_ touch: UITouch, with event: UIEvent?) -> Bool {
        guard isEnabled else { return false }
        guestPressActive = true
        isHighlighted = true
        onPress()
        return true
    }
    override func endTracking(_ touch: UITouch?, with event: UIEvent?) { releasePress() }
    override func continueTracking(_ touch: UITouch, with event: UIEvent?) -> Bool { guestPressActive }
    override func cancelTracking(with event: UIEvent?) { releasePress() }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { releasePress() }
    }
    override func accessibilityActivate() -> Bool {
        guard isEnabled else { return false }
        onPress()
        onRelease()
        return true
    }
    @objc func releasePress() {
        guard guestPressActive else { return }
        guestPressActive = false
        isHighlighted = false
        onRelease()
    }
}
