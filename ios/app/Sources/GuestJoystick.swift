import SwiftUI
import UIKit

/// A held touch owns the WASD keys. The floating pad never intercepts touches.
struct GuestJoystick: UIViewRepresentable {
    let active: Bool

    func makeUIView(context: Context) -> GuestJoystickControl { GuestJoystickControl() }
    func updateUIView(_ view: GuestJoystickControl, context: Context) {
        view.isEnabled = active
        if !active { view.releaseKeys() }
    }
    static func dismantleUIView(_ view: GuestJoystickControl, coordinator: ()) {
        view.releaseKeys()
    }
}

final class GuestJoystickControl: UIControl {
    private let icon = UIImageView(image: UIImage(systemName: "gamecontroller"))
    private let pad = UIView(frame: CGRect(x: 0, y: 0, width: 88, height: 88))
    private let knob = UIView(frame: CGRect(x: 0, y: 0, width: 34, height: 34))
    private var held = Set<String>()
    private var origin = CGPoint.zero

    init() {
        super.init(frame: .zero)
        accessibilityLabel = "WASD joystick"
        accessibilityHint = "Hold and slide to move. Release to stop."
        accessibilityTraits = .button
        isAccessibilityElement = true
        icon.contentMode = .scaleAspectFit
        icon.isUserInteractionEnabled = false
        addSubview(icon)
        pad.backgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.94)
        pad.layer.cornerRadius = 44
        pad.layer.borderWidth = 2
        pad.layer.borderColor = UIColor.systemBlue.cgColor
        pad.isUserInteractionEnabled = false
        knob.backgroundColor = .systemBlue
        knob.layer.cornerRadius = 17
        pad.addSubview(knob)
        NotificationCenter.default.addObserver(self, selector: #selector(releaseKeys),
            name: UIApplication.willResignActiveNotification, object: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    deinit { NotificationCenter.default.removeObserver(self) }

    override func layoutSubviews() {
        super.layoutSubviews()
        icon.frame = CGRect(x: bounds.midX - 12, y: bounds.midY - 12, width: 24, height: 24)
    }
    override func beginTracking(_ touch: UITouch, with event: UIEvent?) -> Bool {
        guard isEnabled, let window else { return false }
        origin = touch.location(in: window)
        pad.center = origin
        knob.center = CGPoint(x: 44, y: 44)
        window.addSubview(pad)
        return true
    }
    override func continueTracking(_ touch: UITouch, with event: UIEvent?) -> Bool {
        guard let window else { releaseKeys(); return false }
        let point = touch.location(in: window)
        let dx = point.x - origin.x, dy = point.y - origin.y
        let distance = hypot(dx, dy)
        let scale = distance > 24 ? 24 / distance : 1
        knob.center = CGPoint(x: 44 + dx * scale, y: 44 + dy * scale)
        var next = Set<String>()
        if distance > 6 {
            if dx > max(6, abs(dy) * 0.45) { next.insert("D") }
            if dx < -max(6, abs(dy) * 0.45) { next.insert("A") }
            if dy > max(6, abs(dx) * 0.45) { next.insert("S") }
            if dy < -max(6, abs(dx) * 0.45) { next.insert("W") }
        }
        for key in held.subtracting(next) { BVNGuestControlsSetKeyNamed(key, false) }
        for key in next.subtracting(held) { BVNGuestControlsSetKeyNamed(key, true) }
        held = next
        return true
    }
    override func endTracking(_ touch: UITouch?, with event: UIEvent?) { releaseKeys() }
    override func cancelTracking(with event: UIEvent?) { releaseKeys() }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { releaseKeys() }
    }
    @objc func releaseKeys() {
        for key in held { BVNGuestControlsSetKeyNamed(key, false) }
        held.removeAll()
        pad.removeFromSuperview()
    }
}
