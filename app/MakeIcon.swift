// Artwork for Tether.app's icon. Used by `make app` in one of two ways:
//
//   makeicon --glyph <out.png>   the white chain glyph alone, on a transparent
//                                1024 x 1024 canvas: the layer image for the
//                                Liquid Glass icon in app/Tether.icon, which
//                                Icon Composer's `ictool` renders
//   makeicon --pad <tile.png> <out.iconset>
//                                every .iconset size from a full-bleed tile
//                                (ictool's rendering), placed on the macOS
//                                icon grid: an 824-pt tile on a 1024 canvas
//   makeicon <out.iconset>       a complete flat icon, for when Icon Composer
//                                is not installed
//
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit

func symbol(pointSize: CGFloat) -> NSImage? {
    let config = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .medium)
        .applying(NSImage.SymbolConfiguration(paletteColors: [.white]))
    return NSImage(systemSymbolName: "personalhotspot", accessibilityDescription: nil)?
        .withSymbolConfiguration(config)
}

func bitmap(pixels: Int) -> NSBitmapImageRep {
    NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                     bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                     isPlanar: false, colorSpaceName: .deviceRGB,
                     bytesPerRow: 0, bitsPerPixel: 0)!
}

if CommandLine.arguments.count == 3 && CommandLine.arguments[1] == "--glyph" {
    let rep = bitmap(pixels: 1024)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    if let glyph = symbol(pointSize: 470) {
        let size = glyph.size
        glyph.draw(in: NSRect(x: 512 - size.width / 2, y: 512 - size.height / 2,
                              width: size.width, height: size.height))
    }
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!
        .write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
    exit(0)
}

let iconsetSizes: [(name: String, pixels: Int)] = [16, 32, 128, 256, 512].flatMap { base in
    [("icon_\(base)x\(base).png", base), ("icon_\(base)x\(base)@2x.png", base * 2)]
}

if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "--pad" {
    guard let tile = NSImage(contentsOfFile: CommandLine.arguments[2]) else {
        fputs("makeicon: cannot read \(CommandLine.arguments[2])\n", stderr)
        exit(1)
    }
    let dir = CommandLine.arguments[3]
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    for (name, pixels) in iconsetSizes {
        let rep = bitmap(pixels: pixels)
        NSGraphicsContext.saveGraphicsState()
        let ctx = NSGraphicsContext(bitmapImageRep: rep)!
        ctx.imageInterpolation = .high
        NSGraphicsContext.current = ctx
        let p = CGFloat(pixels)
        tile.draw(in: NSRect(x: p * 100 / 1024, y: p * 100 / 1024,
                             width: p * 824 / 1024, height: p * 824 / 1024))
        NSGraphicsContext.restoreGraphicsState()
        try! rep.representation(using: .png, properties: [:])!
            .write(to: URL(fileURLWithPath: "\(dir)/\(name)"))
    }
    exit(0)
}

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Tether.iconset"
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

/// Draws the icon on a 1024-point canvas, scaled to `pixels`.
func render(pixels: Int) -> Data {
    let rep = bitmap(pixels: pixels)
    NSGraphicsContext.saveGraphicsState()
    let ctx = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.current = ctx
    let s = CGFloat(pixels) / 1024
    ctx.cgContext.scaleBy(x: s, y: s)

    // macOS icon grid: a 824-pt rounded square centred on the canvas.
    let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
    let shape = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)

    // Drop shadow under the tile.
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.35)
    shadow.shadowOffset = NSSize(width: 0, height: -12)
    shadow.shadowBlurRadius = 28
    shadow.set()
    NSColor.black.setFill()
    shape.fill()
    NSGraphicsContext.restoreGraphicsState()

    // Teal to deep blue.
    NSGradient(colors: [NSColor(srgbRed: 0.16, green: 0.84, blue: 0.78, alpha: 1),
                        NSColor(srgbRed: 0.10, green: 0.42, blue: 0.95, alpha: 1),
                        NSColor(srgbRed: 0.20, green: 0.18, blue: 0.70, alpha: 1)])!
        .draw(in: shape, angle: -70)

    // Soft highlight on the upper half.
    NSGraphicsContext.saveGraphicsState()
    shape.addClip()
    NSGradient(colors: [NSColor.white.withAlphaComponent(0.22), NSColor.white.withAlphaComponent(0)])!
        .draw(in: NSRect(x: 100, y: 512, width: 824, height: 412), angle: -90)
    NSGraphicsContext.restoreGraphicsState()

    // The glyph: the linked-chain "personal hotspot" symbol, in white.
    // Drawn in pixel coordinates: symbol images rasterise at the point size
    // they are drawn at, ignoring the scale above, so undo it first.
    ctx.cgContext.scaleBy(x: 1 / s, y: 1 / s)
    if let glyph = symbol(pointSize: 430 * s) {
        let size = glyph.size
        let mid = 512 * s
        let rect = NSRect(x: mid - size.width / 2, y: mid - size.height / 2,
                          width: size.width, height: size.height)
        NSGraphicsContext.saveGraphicsState()
        let glow = NSShadow()
        glow.shadowColor = NSColor.black.withAlphaComponent(0.25)
        glow.shadowOffset = NSSize(width: 0, height: -8 * s)
        glow.shadowBlurRadius = 16 * s
        glow.set()
        glyph.draw(in: rect)
        NSGraphicsContext.restoreGraphicsState()
    }

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for (name, pixels) in iconsetSizes {
    try! render(pixels: pixels).write(to: URL(fileURLWithPath: "\(outDir)/\(name)"))
}
