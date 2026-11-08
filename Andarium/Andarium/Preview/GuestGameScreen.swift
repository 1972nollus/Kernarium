// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// A stand-in for a game in play: a bright sky with the sun and clouds where
/// the stats capsule sits, dark branches and a score beside it, and a busy
/// level below, so a capture shows the capsule over the hardest ground. The
/// frame is drawn from its size alone and holds still.
struct GuestGameScreen: View {
    var body: some View {
        Canvas { context, size in
            let w = size.width
            let h = size.height
            let horizon = h * 0.68

            // Sky, brightest at the top where the capsule is.
            let sky = Gradient(colors: [Color(hex: 0x3AA8FF), Color(hex: 0x9EDCFF), Color(hex: 0xFFE3A3), Color(hex: 0xFF9F5A)])
            context.fill(
                Path(CGRect(x: 0, y: 0, width: w, height: horizon)),
                with: .linearGradient(sky, startPoint: .zero, endPoint: CGPoint(x: 0, y: horizon))
            )

            // Rays and the sun just under the capsule.
            let sun = CGPoint(x: w * 0.52, y: 92)
            for index in 0..<12 {
                let angle = Double(index) * .pi / 6
                var ray = Path()
                ray.move(to: sun)
                ray.addLine(to: CGPoint(x: sun.x + cos(angle - 0.09) * w, y: sun.y + sin(angle - 0.09) * w))
                ray.addLine(to: CGPoint(x: sun.x + cos(angle + 0.09) * w, y: sun.y + sin(angle + 0.09) * w))
                ray.closeSubpath()
                context.fill(ray, with: .color(.white.opacity(0.16)))
            }
            context.fill(
                Path(ellipseIn: CGRect(x: sun.x - 70, y: sun.y - 70, width: 140, height: 140)),
                with: .radialGradient(
                    Gradient(colors: [.white, Color(hex: 0xFFF4B0), Color(hex: 0xFFF4B0).opacity(0)]),
                    center: sun, startRadius: 0, endRadius: 70
                )
            )

            // Clouds across the top.
            for (x, y, scale) in [(0.18, 70.0, 1.0), (0.62, 54.0, 1.3), (0.9, 110.0, 0.9), (0.38, 150.0, 0.8)] {
                cloud(in: context, at: CGPoint(x: w * x, y: y), scale: scale)
            }

            // Dark branches from the top corners, into the capsule's row.
            branch(in: context, from: CGPoint(x: -10, y: 30), to: CGPoint(x: w * 0.36, y: 78))
            branch(in: context, from: CGPoint(x: w + 10, y: 20), to: CGPoint(x: w * 0.7, y: 66))

            // Mountains.
            var far = Path()
            far.move(to: CGPoint(x: 0, y: horizon))
            for step in 0...8 {
                let x = w * Double(step) / 8
                far.addLine(to: CGPoint(x: x, y: horizon - (step.isMultiple(of: 2) ? 150 : 70)))
            }
            far.addLine(to: CGPoint(x: w, y: horizon))
            context.fill(far, with: .color(Color(hex: 0x7A5BB5).opacity(0.85)))

            // Ground and grass stripes.
            context.fill(Path(CGRect(x: 0, y: horizon, width: w, height: h - horizon)), with: .color(Color(hex: 0x3F9E3A)))
            for stripe in stride(from: 0.0, to: w, by: 28) {
                context.fill(
                    Path(CGRect(x: stripe, y: horizon, width: 14, height: h - horizon)),
                    with: .color(Color(hex: 0x56B84C))
                )
            }
            context.fill(Path(CGRect(x: 0, y: horizon + 26, width: w, height: h - horizon - 26)), with: .color(Color(hex: 0x8A5A2B)))

            // Platforms, coins, the player and an enemy.
            for (x, y, width) in [(0.08, 0.5, 0.3), (0.55, 0.42, 0.34), (0.3, 0.3, 0.22)] {
                let rect = CGRect(x: w * x, y: h * y, width: w * width, height: 18)
                context.fill(Path(roundedRect: rect, cornerRadius: 5), with: .color(Color(hex: 0xC4632A)))
                context.fill(Path(CGRect(x: rect.minX, y: rect.minY, width: rect.width, height: 5)), with: .color(Color(hex: 0x6ED35F)))
                for coin in 0..<3 {
                    let center = CGPoint(x: rect.minX + rect.width * (0.25 + 0.25 * Double(coin)), y: rect.minY - 22)
                    context.fill(Path(ellipseIn: CGRect(x: center.x - 8, y: center.y - 8, width: 16, height: 16)), with: .color(Color(hex: 0xFFC925)))
                    context.stroke(Path(ellipseIn: CGRect(x: center.x - 8, y: center.y - 8, width: 16, height: 16)), with: .color(Color(hex: 0xC98A00)), lineWidth: 2)
                }
            }
            let player = CGRect(x: w * 0.2, y: horizon - 44, width: 30, height: 44)
            context.fill(Path(roundedRect: player, cornerRadius: 8), with: .color(Color(hex: 0xE5322D)))
            context.fill(Path(ellipseIn: CGRect(x: player.midX - 2, y: player.minY + 8, width: 10, height: 10)), with: .color(.white))
            let enemy = CGRect(x: w * 0.7, y: horizon - 28, width: 36, height: 28)
            context.fill(Path(roundedRect: enemy, cornerRadius: 14), with: .color(Color(hex: 0x5A2E8C)))

            // The game's own score and lives on either side of the capsule.
            context.draw(
                Text("SCORE 128450").font(.system(size: 17, weight: .heavy, design: .rounded)).foregroundStyle(.white),
                at: CGPoint(x: 18, y: 32), anchor: .leading
            )
            context.draw(
                Text("\u{2665}\u{2665}\u{2665}").font(.system(size: 20, weight: .bold)).foregroundStyle(Color(hex: 0xFF3B5C)),
                at: CGPoint(x: w - 18, y: 32), anchor: .trailing
            )
        }
        .ignoresSafeArea()
    }

    private func cloud(in context: GraphicsContext, at center: CGPoint, scale: Double) {
        for (dx, dy, r) in [(-34.0, 6.0, 22.0), (-8.0, -10.0, 30.0), (24.0, 0.0, 24.0), (46.0, 10.0, 16.0)] {
            let radius = r * scale
            let rect = CGRect(x: center.x + dx * scale - radius, y: center.y + dy * scale - radius, width: radius * 2, height: radius * 2)
            context.fill(Path(ellipseIn: rect), with: .color(.white.opacity(0.95)))
        }
    }

    private func branch(in context: GraphicsContext, from start: CGPoint, to end: CGPoint) {
        var path = Path()
        path.move(to: start)
        path.addQuadCurve(to: end, control: CGPoint(x: (start.x + end.x) / 2, y: end.y + 30))
        context.stroke(path, with: .color(Color(hex: 0x2B1D12)), style: StrokeStyle(lineWidth: 9, lineCap: .round))
        for t in [0.35, 0.6, 0.85] {
            let point = CGPoint(x: start.x + (end.x - start.x) * t, y: start.y + (end.y - start.y) * t + 12)
            context.fill(Path(ellipseIn: CGRect(x: point.x - 16, y: point.y - 10, width: 32, height: 20)), with: .color(Color(hex: 0x1F5A24)))
        }
    }
}
