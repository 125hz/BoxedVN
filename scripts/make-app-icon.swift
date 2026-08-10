#!/usr/bin/env swift
//
//  BoxedVN - iOS/iPadOS port of Boxedwine
//  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
//  Draws ios/app/Resources/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png.
//
//  The icon is generated rather than hand-drawn so it stays reproducible and
//  reviewable as a diff: the committed PNG is a build product of this file, and
//  changing the artwork means changing code, not swapping an opaque binary.
//
//    swift scripts/make-app-icon.swift
//
//  The subject is a visual-novel text box over a 4:3 screen, which is what this
//  app is for.  There is no lettering: at the 40pt size iOS actually draws most
//  of the time, type turns to mush.
//

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let side = 1024
let repoRoot = URL(fileURLWithPath: CommandLine.arguments.first ?? ".")
    .deletingLastPathComponent()   // scripts/
    .deletingLastPathComponent()   // repo root
let outputURL = repoRoot
    .appendingPathComponent("ios/app/Resources/Assets.xcassets")
    .appendingPathComponent("AppIcon.appiconset/AppIcon-1024.png")

guard let space = CGColorSpace(name: CGColorSpace.sRGB),
      let context = CGContext(data: nil,
                              width: side,
                              height: side,
                              bitsPerComponent: 8,
                              bytesPerRow: 0,
                              space: space,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
else {
    FileHandle.standardError.write(Data("Could not create the drawing context.\n".utf8))
    exit(1)
}

func rgb(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1.0) -> CGColor {
    CGColor(colorSpace: space, components: [r / 255.0, g / 255.0, b / 255.0, a])!
}

let s = CGFloat(side)

// A full-bleed background: iOS masks the corners itself, and anything the app
// draws inside the mask radius is wasted.
let backdrop = CGGradient(colorsSpace: space,
                          colors: [rgb(26, 20, 48), rgb(72, 28, 78), rgb(140, 44, 74)] as CFArray,
                          locations: [0.0, 0.55, 1.0])!
context.drawLinearGradient(backdrop,
                           start: CGPoint(x: 0, y: s),
                           end: CGPoint(x: s, y: 0),
                           options: [])

// A soft glow behind the screen, so the flat panel below has something to sit
// on and the icon does not read as a sticker.
let glow = CGGradient(colorsSpace: space,
                      colors: [rgb(255, 190, 210, 0.22), rgb(255, 190, 210, 0.0)] as CFArray,
                      locations: [0.0, 1.0])!
context.drawRadialGradient(glow,
                           startCenter: CGPoint(x: s * 0.5, y: s * 0.62),
                           startRadius: 0,
                           endCenter: CGPoint(x: s * 0.5, y: s * 0.62),
                           endRadius: s * 0.46,
                           options: [])

// The 4:3 screen. Its proportions are the whole point of the app's presentation
// work, so they are exact rather than eyeballed.
let screenWidth = s * 0.615
let screenHeight = screenWidth * 3.0 / 4.0
let screen = CGRect(x: (s - screenWidth) / 2.0,
                    y: (s - screenHeight) / 2.0 - s * 0.004,
                    width: screenWidth,
                    height: screenHeight)
let screenPath = CGPath(roundedRect: screen,
                        cornerWidth: s * 0.045,
                        cornerHeight: s * 0.045,
                        transform: nil)

context.saveGState()
context.addPath(screenPath)
context.clip()
// A night sky, darkest at the top and warming towards the horizon.
let sky = CGGradient(colorsSpace: space,
                     colors: [rgb(14, 12, 34), rgb(46, 26, 66), rgb(120, 52, 84)] as CFArray,
                     locations: [0.0, 0.55, 1.0])!
context.drawLinearGradient(sky,
                           start: CGPoint(x: screen.midX, y: screen.maxY),
                           end: CGPoint(x: screen.midX, y: screen.minY),
                           options: [])

// A moon, high and to one side of the figure below.
context.setFillColor(rgb(255, 230, 202, 0.94))
context.fillEllipse(in: CGRect(x: screen.minX + screen.width * 0.70,
                               y: screen.minY + screen.height * 0.68,
                               width: screen.width * 0.14,
                               height: screen.width * 0.14))

// A character silhouette, cropped at the shoulders by the text box below it.
// This is the one shape that makes the icon read as a visual novel rather than
// as a generic screen or a form.
let figureCenterX = screen.minX + screen.width * 0.40
let headRadius = screen.width * 0.098
let headCenterY = screen.minY + screen.height * 0.575
context.setFillColor(rgb(12, 9, 26, 0.94))
context.fillEllipse(in: CGRect(x: figureCenterX - headRadius,
                               y: headCenterY - headRadius,
                               width: headRadius * 2.0,
                               height: headRadius * 2.2))

let shoulderTop = headCenterY - headRadius * 1.05
let shoulderBottom = screen.minY
let shoulderHalfTop = headRadius * 1.18
let shoulderHalfBottom = headRadius * 2.65
context.move(to: CGPoint(x: figureCenterX - shoulderHalfBottom, y: shoulderBottom))
context.addCurve(to: CGPoint(x: figureCenterX - shoulderHalfTop, y: shoulderTop),
                 control1: CGPoint(x: figureCenterX - shoulderHalfBottom,
                                   y: shoulderBottom + (shoulderTop - shoulderBottom) * 0.55),
                 control2: CGPoint(x: figureCenterX - shoulderHalfTop * 1.5,
                                   y: shoulderTop))
context.addLine(to: CGPoint(x: figureCenterX + shoulderHalfTop, y: shoulderTop))
context.addCurve(to: CGPoint(x: figureCenterX + shoulderHalfBottom, y: shoulderBottom),
                 control1: CGPoint(x: figureCenterX + shoulderHalfTop * 1.5,
                                   y: shoulderTop),
                 control2: CGPoint(x: figureCenterX + shoulderHalfBottom,
                                   y: shoulderBottom + (shoulderTop - shoulderBottom) * 0.55))
context.closePath()
context.fillPath()

// The text box, in the lower third where a visual novel puts it.
let boxInset = screen.width * 0.075
let box = CGRect(x: screen.minX + boxInset,
                 y: screen.minY + screen.height * 0.10,
                 width: screen.width - boxInset * 2.0,
                 height: screen.height * 0.40)
context.setFillColor(rgb(10, 8, 24, 0.90))
context.addPath(CGPath(roundedRect: box,
                       cornerWidth: s * 0.018,
                       cornerHeight: s * 0.018,
                       transform: nil))
context.fillPath()
context.setStrokeColor(rgb(255, 255, 255, 0.24))
context.setLineWidth(s * 0.006)
context.addPath(CGPath(roundedRect: box,
                       cornerWidth: s * 0.018,
                       cornerHeight: s * 0.018,
                       transform: nil))
context.strokePath()

// Speaker chip, straddling the top edge of the box the way most engines draw it.
let chip = CGRect(x: box.minX + box.width * 0.05,
                  y: box.maxY - s * 0.021,
                  width: box.width * 0.30,
                  height: s * 0.042)
context.setFillColor(rgb(236, 92, 122))
context.addPath(CGPath(roundedRect: chip,
                       cornerWidth: chip.height / 2.0,
                       cornerHeight: chip.height / 2.0,
                       transform: nil))
context.fillPath()

// Two lines of "dialogue". Two, not three: a third line survives 1024px and
// disappears at 40px, which makes the icon look smudged rather than detailed.
context.setFillColor(rgb(240, 236, 250, 0.93))
let lineHeight = s * 0.030
for (index, fraction) in [1.0, 0.62].enumerated() {
    let line = CGRect(x: box.minX + box.width * 0.06,
                      y: box.maxY - box.height * (0.42 + Double(index) * 0.30),
                      width: (box.width * 0.88) * fraction,
                      height: lineHeight)
    context.addPath(CGPath(roundedRect: line,
                           cornerWidth: lineHeight / 2.0,
                           cornerHeight: lineHeight / 2.0,
                           transform: nil))
    context.fillPath()
}

// The advance marker.
let marker = s * 0.030
context.setFillColor(rgb(236, 92, 122))
context.move(to: CGPoint(x: box.maxX - box.width * 0.06 - marker,
                         y: box.minY + box.height * 0.20 + marker))
context.addLine(to: CGPoint(x: box.maxX - box.width * 0.06,
                            y: box.minY + box.height * 0.20 + marker))
context.addLine(to: CGPoint(x: box.maxX - box.width * 0.06 - marker / 2.0,
                            y: box.minY + box.height * 0.20))
context.fillPath()

context.restoreGState()

// Screen bezel, drawn last so it sits over the clipped contents.
context.setStrokeColor(rgb(255, 255, 255, 0.30))
context.setLineWidth(s * 0.010)
context.addPath(screenPath)
context.strokePath()

guard let image = context.makeImage() else {
    FileHandle.standardError.write(Data("Could not render the icon.\n".utf8))
    exit(1)
}

try? FileManager.default.createDirectory(
    at: outputURL.deletingLastPathComponent(), withIntermediateDirectories: true)

guard let destination = CGImageDestinationCreateWithURL(
    outputURL as CFURL, UTType.png.identifier as CFString, 1, nil) else {
    FileHandle.standardError.write(Data("Could not open \(outputURL.path).\n".utf8))
    exit(1)
}
CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else {
    FileHandle.standardError.write(Data("Could not write \(outputURL.path).\n".utf8))
    exit(1)
}

print("Wrote \(outputURL.path) (\(side)x\(side))")
