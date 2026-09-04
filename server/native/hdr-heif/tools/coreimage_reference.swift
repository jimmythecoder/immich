// Ground-truth validation helper (development only).
//
// Renders a HEIC through Apple CoreImage with `expandToHDR`, which is the
// authoritative interpretation of Apple's gain map, and reports the same
// statistics the C helper reports. If our reconstruction is correct the two
// should agree closely.
//
//   swiftc -O coreimage_reference.swift -o coreimage_reference
//   ./coreimage_reference <input.heic> [longEdge]

import CoreImage
import Foundation

let arguments = CommandLine.arguments
guard arguments.count >= 2 else {
    FileHandle.standardError.write("usage: coreimage_reference <input.heic> [longEdge]\n".data(using: .utf8)!)
    exit(1)
}

let url = URL(fileURLWithPath: arguments[1])
let longEdge = arguments.count >= 3 ? Double(arguments[2]) ?? 1440 : 1440
// Optional third argument: write raw float32 RGB (linear Display P3) here so the
// C helper's reconstruction can be diffed against CoreImage pixel by pixel.
let dumpPath: String? = arguments.count >= 4 ? arguments[3] : nil
// Set REFERENCE_SDR=1 to render without expandToHDR, which isolates differences
// in the base (SDR) rendering from differences in gain-map interpretation.
let expandToHDR = ProcessInfo.processInfo.environment["REFERENCE_SDR"] != "1"

guard var image = CIImage(contentsOf: url, options: [.expandToHDR: true]) else {
    FileHandle.standardError.write("could not open image\n".data(using: .utf8)!)
    exit(2)
}

let extent = image.extent
let scale = min(1.0, longEdge / Double(max(extent.width, extent.height)))
if scale < 1.0 {
    image = image.transformed(by: CGAffineTransform(scaleX: CGFloat(scale), y: CGFloat(scale)))
}

let bounds = image.extent.integral
let width = Int(bounds.width)
let height = Int(bounds.height)

// Extended-range linear Display P3: values above 1.0 are genuine HDR headroom,
// with 1.0 defined as diffuse SDR white. This matches the C helper's internal
// representation exactly.
guard let colorSpace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3) else {
    FileHandle.standardError.write("no extended linear Display P3 colour space\n".data(using: .utf8)!)
    exit(3)
}

let context = CIContext(options: [.workingColorSpace: colorSpace, .workingFormat: CIFormat.RGBAf])
let rowBytes = width * 4 * MemoryLayout<Float>.size
var buffer = [Float](repeating: 0, count: width * height * 4)

buffer.withUnsafeMutableBytes { raw in
    context.render(
        image,
        toBitmap: raw.baseAddress!,
        rowBytes: rowBytes,
        bounds: bounds,
        format: .RGBAf,
        colorSpace: colorSpace
    )
}

let sdrWhiteNits = 203.0
var peak = 0.0
var aboveSdr = 0
var total = 0
var luminanceSum = 0.0

for pixel in stride(from: 0, to: buffer.count, by: 4) {
    let r = Double(buffer[pixel])
    let g = Double(buffer[pixel + 1])
    let b = Double(buffer[pixel + 2])
    let channelPeak = max(r, max(g, b))

    if channelPeak > peak { peak = channelPeak }
    if channelPeak > 1.05 { aboveSdr += 1 }
    luminanceSum += 0.2627 * r + 0.6780 * g + 0.0593 * b
    total += 1
}

let fraction = total > 0 ? Double(aboveSdr) / Double(total) : 0
let average = total > 0 ? luminanceSum / Double(total) : 0

if let dumpPath {
    // Drop alpha so the layout matches the C helper's float32 RGB dump.
    var rgb = [Float]()
    rgb.reserveCapacity(width * height * 3)
    for pixel in stride(from: 0, to: buffer.count, by: 4) {
        rgb.append(buffer[pixel])
        rgb.append(buffer[pixel + 1])
        rgb.append(buffer[pixel + 2])
    }
    let data = rgb.withUnsafeBufferPointer { Data(buffer: $0) }
    try data.write(to: URL(fileURLWithPath: dumpPath))
}

let json = """
{"ok":true,"source":"coreimage","width":\(width),"height":\(height),\
"peakRelative":\(String(format: "%.4f", peak)),\
"peakNits":\(String(format: "%.3f", peak * sdrWhiteNits)),\
"fractionAboveSdrWhite":\(String(format: "%.6f", fraction)),\
"averageNits":\(String(format: "%.3f", average * sdrWhiteNits))}
"""
print(json)
