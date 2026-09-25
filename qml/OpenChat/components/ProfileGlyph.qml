import QtQuick
import QtQuick.Shapes
import OpenChat

// The profile pages' small line glyphs (envelope, phone, lock, …): vector
// paths drawn with Shape, recoloured at run time, so no icon font and no
// painted raster. Each kind is one stroked path and one filled path in a unit
// square, scaled to the item's smaller side and centred in it.
Item {
    id: glyph
    // chevron, back, pencil, eye, eyeOff, lock, camera, person, shield, check,
    // note, music, copy, idCard, envelope, phone, video, palette, grip, undo,
    // redo, plus, minus, cross, down, up, swap, image, star, text, sparkle,
    // layout, boxes, friends, link, upload, play, pause, preview, dice,
    // shieldCheck, gamepad, film, heart, book, chat, trophy, file, download.
    // An unknown kind draws nothing.
    property string kind: "envelope"
    property color ink: Theme.iconInk
    // Line width in pixels; a few kinds ask for a heavier line of their own.
    property real stroke: Math.max(1.4, size / 12)
    readonly property real size: Math.min(width, height)
    readonly property var art: glyph.draw(glyph.kind, glyph.size)

    implicitWidth: 18
    implicitHeight: 18
    Accessible.ignored: true

    Shape {
        // Smooth on the GPU as well: the default geometry renderer draws
        // shapes without antialiasing there, which chips a small glyph's
        // points and shifts its weight (the software renderer ignores this).
        preferredRendererType: Shape.CurveRenderer
        x: (glyph.width - glyph.size) / 2
        y: (glyph.height - glyph.size) / 2
        width: glyph.size
        height: glyph.size

        ShapePath {
            strokeColor: glyph.art.stroke.length > 0 ? glyph.ink : "transparent"
            strokeWidth: Math.max(glyph.stroke, glyph.art.weight)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: glyph.art.stroke }
        }
        ShapePath {
            strokeColor: "transparent"
            strokeWidth: 0
            fillColor: glyph.art.fill.length > 0 ? glyph.ink : "transparent"
            fillRule: ShapePath.WindingFill
            PathSvg { path: glyph.art.fill }
        }
    }

    // SVG path data for `kind` at `s` pixels: {stroke, fill, weight}.
    function draw(kind, s) {
        if (!(s > 0))
            return { stroke: "", fill: "", weight: 0 };
        // Rotated kinds map their centred coordinates through this.
        var turn = null;
        function f(v) { return (v * s).toFixed(2); }
        function pt(x, y) {
            if (turn) {
                var c = Math.cos(turn), n = Math.sin(turn);
                var rx = 0.5 + x * c - y * n, ry = 0.5 + x * n + y * c;
                x = rx; y = ry;
            }
            return f(x) + " " + f(y);
        }
        function M(x, y) { return " M " + pt(x, y); }
        function L(x, y) { return " L " + pt(x, y); }
        function Q(cx, cy, x, y) { return " Q " + pt(cx, cy) + " " + pt(x, y); }
        function C(ax, ay, bx, by, x, y) { return " C " + pt(ax, ay) + " " + pt(bx, by) + " " + pt(x, y); }
        var Z = " Z";
        function rect(x, y, w, h) { return M(x, y) + L(x + w, y) + L(x + w, y + h) + L(x, y + h) + Z; }
        function rr(x, y, w, h, r) {
            return M(x + r, y) + L(x + w - r, y) + Q(x + w, y, x + w, y + r) + L(x + w, y + h - r)
                 + Q(x + w, y + h, x + w - r, y + h) + L(x + r, y + h) + Q(x, y + h, x, y + h - r)
                 + L(x, y + r) + Q(x, y, x + r, y) + Z;
        }
        function ellipse(cx, cy, rx, ry) {
            return M(cx - rx, cy) + " A " + f(rx) + " " + f(ry) + " 0 1 0 " + pt(cx + rx, cy)
                 + " A " + f(rx) + " " + f(ry) + " 0 1 0 " + pt(cx - rx, cy) + Z;
        }
        function circle(cx, cy, r) { return ellipse(cx, cy, r, r); }
        // An arc from angle a0 to a1 (radians; screen coordinates, y down).
        function arc(cx, cy, r, a0, a1, clockwise) {
            return M(cx + r * Math.cos(a0), cy + r * Math.sin(a0)) + " A " + f(r) + " " + f(r) + " 0 "
                 + (Math.abs(a1 - a0) > Math.PI ? 1 : 0) + " " + (clockwise ? 1 : 0) + " "
                 + pt(cx + r * Math.cos(a1), cy + r * Math.sin(a1));
        }
        function star(cx, cy, r, inner, points) {
            var p = "";
            for (var i = 0; i < points * 2; ++i) {
                var radius = i % 2 === 0 ? r : r * inner;
                var a = -Math.PI / 2 + i * Math.PI / points;
                p += (i === 0 ? M : L)(cx + Math.cos(a) * radius, cy + Math.sin(a) * radius);
            }
            return p + Z;
        }
        function sparkle(cx, cy, r) {
            var k = r * 0.12;
            return M(cx, cy - r) + Q(cx + k, cy - k, cx + r, cy) + Q(cx + k, cy + k, cx, cy + r)
                 + Q(cx - k, cy + k, cx - r, cy) + Q(cx - k, cy - k, cx, cy - r) + Z;
        }
        // A straight bar of thickness t, filled (note stems and beams).
        function bar(x1, y1, x2, y2, t) {
            var dx = x2 - x1, dy = y2 - y1, len = Math.sqrt(dx * dx + dy * dy) || 1;
            var nx = -dy / len * t / 2, ny = dx / len * t / 2;
            return M(x1 + nx, y1 + ny) + L(x2 + nx, y2 + ny) + L(x2 - nx, y2 - ny) + L(x1 - nx, y1 - ny) + Z;
        }
        function eye() { return M(0.06, 0.5) + Q(0.5, 0.08, 0.94, 0.5) + Q(0.5, 0.92, 0.06, 0.5) + Z; }
        function shield() { return M(0.5, 0.06) + L(0.86, 0.2) + Q(0.86, 0.72, 0.5, 0.94) + Q(0.14, 0.72, 0.14, 0.2) + Z; }

        var stroke = "", fill = "", weight = 0;
        switch (kind) {
        case "envelope":
            stroke = rr(0.08, 0.2, 0.84, 0.6, 0.08) + M(0.12, 0.26) + L(0.5, 0.55) + L(0.88, 0.26);
            break;
        case "phone":
            fill = M(0.27, 0.12) + Q(0.12, 0.16, 0.13, 0.32) + C(0.18, 0.62, 0.4, 0.84, 0.7, 0.88)
                 + Q(0.84, 0.88, 0.88, 0.74) + L(0.72, 0.6) + L(0.6, 0.7) + Q(0.42, 0.6, 0.32, 0.42)
                 + L(0.42, 0.3) + Z;
            break;
        case "video":
            fill = rr(0.06, 0.26, 0.58, 0.48, 0.08) + M(0.68, 0.44) + L(0.94, 0.28) + L(0.94, 0.72) + L(0.68, 0.56) + Z;
            break;
        case "shield":
            stroke = shield() + M(0.33, 0.5) + L(0.46, 0.63) + L(0.68, 0.38);
            break;
        case "shieldCheck":
            fill = shield();
            break;
        case "copy":
            stroke = rr(0.3, 0.1, 0.56, 0.62, 0.08) + M(0.18, 0.3) + L(0.14, 0.3) + L(0.14, 0.9) + L(0.62, 0.9)
                   + L(0.62, 0.84);
            break;
        case "pencil":
            turn = Math.PI / 4;
            stroke = rr(-0.12, -0.44, 0.24, 0.66, 0.04) + M(-0.12, 0.22) + L(0, 0.42) + L(0.12, 0.22);
            break;
        case "link":
            turn = -Math.PI / 4;
            stroke = rr(-0.42, -0.15, 0.46, 0.3, 0.15) + rr(-0.04, -0.15, 0.46, 0.3, 0.15);
            break;
        case "camera":
            stroke = rr(0.08, 0.28, 0.84, 0.56, 0.1) + M(0.32, 0.28) + L(0.4, 0.16) + L(0.6, 0.16) + L(0.68, 0.28)
                   + circle(0.5, 0.56, 0.16);
            break;
        case "back":
            stroke = M(0.62, 0.2) + L(0.32, 0.5) + L(0.62, 0.8);
            weight = Math.max(1.8, s / 9);
            break;
        case "chevron":
            stroke = M(0.4, 0.22) + L(0.68, 0.5) + L(0.4, 0.78);
            weight = Math.max(1.6, s / 10);
            break;
        case "play":
            // Its centroid, not its box, sits in the middle: that is where
            // the eye puts a triangle's centre, so callers never nudge it.
            fill = M(0.34, 0.2) + L(0.82, 0.5) + L(0.34, 0.8) + Z;
            break;
        case "pause":
            fill = rect(0.28, 0.2, 0.16, 0.6) + rect(0.56, 0.2, 0.16, 0.6);
            break;
        case "note":
            // Two beamed notes: MySpace's "now playing" mark.
            fill = ellipse(0.29, 0.77, 0.17, 0.125) + ellipse(0.73, 0.69, 0.17, 0.125)
                 + bar(0.415, 0.77, 0.415, 0.18, 0.085) + bar(0.855, 0.69, 0.855, 0.1, 0.085)
                 + M(0.373, 0.15) + L(0.897, 0.07) + L(0.897, 0.23) + L(0.373, 0.31) + Z;
            break;
        case "music":
            fill = ellipse(0.38, 0.74, 0.2, 0.15) + bar(0.54, 0.74, 0.54, 0.12, 0.09);
            stroke = M(0.54, 0.14) + Q(0.86, 0.26, 0.76, 0.54);
            break;
        case "person":
            fill = circle(0.5, 0.34, 0.18) + M(0.16, 0.9) + Q(0.18, 0.56, 0.5, 0.56) + Q(0.82, 0.56, 0.84, 0.9) + Z;
            break;
        case "idCard":
        case "idcard":
            stroke = rr(0.06, 0.18, 0.88, 0.64, 0.1) + M(0.58, 0.4) + L(0.82, 0.4) + M(0.58, 0.56) + L(0.76, 0.56);
            fill = circle(0.32, 0.44, 0.1) + M(0.18, 0.7) + Q(0.32, 0.52, 0.46, 0.7) + Z;
            break;
        case "lock":
            fill = rr(0.2, 0.44, 0.6, 0.46, 0.08);
            stroke = arc(0.5, 0.44, 0.2, Math.PI, 2 * Math.PI, true);
            break;
        case "check":
            stroke = M(0.2, 0.52) + L(0.42, 0.74) + L(0.82, 0.28);
            weight = Math.max(1.8, s / 8);
            break;
        case "palette":
            stroke = M(0.5, 0.08) + C(0.1, 0.08, 0.04, 0.5, 0.14, 0.7) + C(0.26, 0.94, 0.5, 0.96, 0.56, 0.84)
                   + C(0.62, 0.72, 0.52, 0.66, 0.62, 0.6) + C(0.74, 0.54, 0.94, 0.66, 0.94, 0.46)
                   + C(0.94, 0.22, 0.74, 0.08, 0.5, 0.08) + Z;
            fill = circle(0.3, 0.36, 0.065) + circle(0.5, 0.25, 0.065) + circle(0.7, 0.34, 0.065)
                 + circle(0.3, 0.6, 0.065);
            break;
        case "image":
            stroke = rr(0.08, 0.16, 0.84, 0.68, 0.08);
            fill = M(0.14, 0.76) + L(0.38, 0.5) + L(0.54, 0.64) + L(0.66, 0.54) + L(0.86, 0.76) + Z
                 + circle(0.68, 0.34, 0.07);
            break;
        case "boxes":
            stroke = rr(0.1, 0.12, 0.8, 0.76, 0.08) + M(0.24, 0.52) + L(0.76, 0.52) + M(0.24, 0.68) + L(0.6, 0.68);
            fill = rect(0.1, 0.12, 0.8, 0.22);
            break;
        case "layout":
        case "columns":
            stroke = rr(0.08, 0.14, 0.32, 0.72, 0.06) + rr(0.48, 0.14, 0.44, 0.32, 0.06) + rr(0.48, 0.54, 0.44, 0.32, 0.06);
            break;
        case "friends":
        case "people":
            fill = circle(0.36, 0.34, 0.14) + M(0.1, 0.84) + Q(0.12, 0.54, 0.36, 0.54) + Q(0.6, 0.54, 0.62, 0.84) + Z
                 + circle(0.7, 0.3, 0.11) + M(0.66, 0.5) + Q(0.9, 0.48, 0.92, 0.76) + L(0.68, 0.76)
                 + Q(0.68, 0.6, 0.6, 0.52) + Z;
            break;
        case "text":
            // "Aa": a capital A and a small a, drawn as lines.
            stroke = M(0.06, 0.8) + L(0.28, 0.2) + L(0.5, 0.8) + M(0.14, 0.6) + L(0.42, 0.6)
                   + circle(0.74, 0.65, 0.14) + M(0.88, 0.48) + L(0.88, 0.8);
            break;
        case "star":
            fill = star(0.5, 0.54, 0.44, 0.46, 5);
            break;
        case "cross":
            stroke = M(0.24, 0.24) + L(0.76, 0.76) + M(0.76, 0.24) + L(0.24, 0.76);
            weight = Math.max(1.8, s / 8);
            break;
        case "plus":
            stroke = M(0.5, 0.18) + L(0.5, 0.82) + M(0.18, 0.5) + L(0.82, 0.5);
            weight = Math.max(1.8, s / 8);
            break;
        case "minus":
            stroke = M(0.18, 0.5) + L(0.82, 0.5);
            weight = Math.max(1.8, s / 8);
            break;
        case "undo":
            stroke = arc(0.54, 0.56, 0.28, 1.05 * Math.PI, 2.35 * Math.PI, true)
                   + M(0.12, 0.38) + L(0.27, 0.6) + L(0.44, 0.42);
            weight = Math.max(1.6, s / 10);
            break;
        case "redo":
            stroke = arc(0.46, 0.56, 0.28, -0.05 * Math.PI, -1.35 * Math.PI, false)
                   + M(0.88, 0.38) + L(0.73, 0.6) + L(0.56, 0.42);
            weight = Math.max(1.6, s / 10);
            break;
        case "eye":
            stroke = eye();
            fill = circle(0.5, 0.5, 0.14);
            break;
        case "eyeOff":
        case "eyeoff":
            stroke = eye() + M(0.14, 0.86) + L(0.86, 0.14);
            break;
        case "grip":
            for (var gy = 0; gy < 3; ++gy) {
                for (var gx = 0; gx < 2; ++gx)
                    fill += circle(0.38 + gx * 0.24, 0.26 + gy * 0.24, 0.07);
            }
            break;
        case "up":
            stroke = M(0.22, 0.62) + L(0.5, 0.34) + L(0.78, 0.62);
            break;
        case "down":
            stroke = M(0.22, 0.38) + L(0.5, 0.66) + L(0.78, 0.38);
            break;
        case "swap":
            stroke = M(0.14, 0.36) + L(0.86, 0.36) + L(0.68, 0.2) + M(0.86, 0.66) + L(0.14, 0.66) + L(0.32, 0.82);
            break;
        case "upload":
            stroke = M(0.5, 0.66) + L(0.5, 0.14) + M(0.28, 0.34) + L(0.5, 0.12) + L(0.72, 0.34)
                   + M(0.14, 0.62) + L(0.14, 0.86) + L(0.86, 0.86) + L(0.86, 0.62);
            break;
        case "download":
            stroke = M(0.5, 0.12) + L(0.5, 0.64) + M(0.28, 0.42) + L(0.5, 0.64) + L(0.72, 0.42)
                   + M(0.14, 0.62) + L(0.14, 0.86) + L(0.86, 0.86) + L(0.86, 0.62);
            break;
        case "file":
            // A sheet with its corner folded down, and two lines of text.
            // The sheet spans 0.21 to 0.79, so its box is centred.
            stroke = M(0.21, 0.14) + Q(0.21, 0.08, 0.27, 0.08) + L(0.57, 0.08) + L(0.79, 0.3)
                   + L(0.79, 0.86) + Q(0.79, 0.92, 0.73, 0.92) + L(0.27, 0.92) + Q(0.21, 0.92, 0.21, 0.86) + Z
                   + M(0.57, 0.08) + L(0.57, 0.3) + L(0.79, 0.3)
                   + M(0.34, 0.54) + L(0.66, 0.54) + M(0.34, 0.72) + L(0.57, 0.72);
            break;
        case "sparkle":
            fill = sparkle(0.42, 0.46, 0.36) + sparkle(0.8, 0.2, 0.16);
            break;
        case "preview":
            stroke = rr(0.06, 0.16, 0.88, 0.68, 0.08);
            fill = rect(0.06, 0.16, 0.3, 0.68);
            break;
        case "dice":
            stroke = rr(0.12, 0.12, 0.76, 0.76, 0.14);
            fill = circle(0.34, 0.34, 0.07) + circle(0.5, 0.5, 0.07) + circle(0.66, 0.66, 0.07);
            break;
        // Custom panels' icons.
        case "gamepad":
            stroke = M(0.3, 0.3) + L(0.7, 0.3) + C(0.9, 0.3, 0.98, 0.62, 0.92, 0.76)
                   + C(0.86, 0.9, 0.72, 0.84, 0.64, 0.68) + L(0.36, 0.68) + C(0.28, 0.84, 0.14, 0.9, 0.08, 0.76)
                   + C(0.02, 0.62, 0.1, 0.3, 0.3, 0.3) + Z;
            fill = rect(0.22, 0.46, 0.18, 0.06) + rect(0.28, 0.4, 0.06, 0.18)
                 + circle(0.66, 0.44, 0.045) + circle(0.76, 0.54, 0.045);
            break;
        case "film":
            stroke = rr(0.1, 0.12, 0.8, 0.76, 0.06) + M(0.26, 0.12) + L(0.26, 0.88) + M(0.74, 0.12) + L(0.74, 0.88);
            fill = rect(0.14, 0.2, 0.08, 0.08) + rect(0.14, 0.46, 0.08, 0.08) + rect(0.14, 0.72, 0.08, 0.08)
                 + rect(0.78, 0.2, 0.08, 0.08) + rect(0.78, 0.46, 0.08, 0.08) + rect(0.78, 0.72, 0.08, 0.08);
            break;
        case "heart":
            fill = M(0.5, 0.86) + C(0.14, 0.62, 0.04, 0.4, 0.14, 0.24) + C(0.24, 0.1, 0.44, 0.12, 0.5, 0.3)
                 + C(0.56, 0.12, 0.76, 0.1, 0.86, 0.24) + C(0.96, 0.4, 0.86, 0.62, 0.5, 0.86) + Z;
            break;
        case "book":
            stroke = M(0.5, 0.24) + Q(0.3, 0.12, 0.08, 0.18) + L(0.08, 0.8) + Q(0.3, 0.74, 0.5, 0.86)
                   + Q(0.7, 0.74, 0.92, 0.8) + L(0.92, 0.18) + Q(0.7, 0.12, 0.5, 0.24) + L(0.5, 0.86);
            break;
        case "chat":
            stroke = rr(0.08, 0.14, 0.84, 0.56, 0.14) + M(0.28, 0.7) + L(0.24, 0.9) + L(0.46, 0.7);
            break;
        case "trophy":
            stroke = M(0.28, 0.14) + L(0.72, 0.14) + L(0.72, 0.36) + Q(0.72, 0.6, 0.5, 0.62) + Q(0.28, 0.6, 0.28, 0.36) + Z
                   + M(0.28, 0.2) + Q(0.08, 0.2, 0.12, 0.36) + Q(0.16, 0.46, 0.3, 0.46)
                   + M(0.72, 0.2) + Q(0.92, 0.2, 0.88, 0.36) + Q(0.84, 0.46, 0.7, 0.46)
                   + M(0.5, 0.62) + L(0.5, 0.78);
            fill = rect(0.32, 0.78, 0.36, 0.1);
            break;
        }
        return { stroke: stroke, fill: fill, weight: weight };
    }
}
