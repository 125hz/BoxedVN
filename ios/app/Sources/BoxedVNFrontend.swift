/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

import SwiftUI
import UIKit

/// The entry point the Objective-C++ app delegate looks up by name.
///
/// BoxedVN does not use SwiftUI's `@main`: SDL owns `UIApplicationMain` so
/// that its UIKit backend behaves the way it expects (see BVNMain.mm).  The
/// delegate calls this to obtain the root view controller for the library
/// window.
@objc(BoxedVNFrontend)
final class BoxedVNFrontend: NSObject {
    /// Called from -[BVNAppDelegate createLibraryWindow], which runs on the
    /// main thread inside application:didFinishLaunchingWithOptions:.
    @MainActor
    @objc static func makeRootViewController() -> UIViewController {
        let model = AppModel()
        return UIHostingController(rootView: RootView().environmentObject(model))
    }
}
