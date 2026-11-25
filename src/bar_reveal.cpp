// A new look pours in from the dock's own edge, as astraeus' does from the panel's: one sheet of liquid glass, its front leading with a soft
// bulge where the pointer was, then levelling out and settling with a last small wobble as it lands. Its lip reads like glass: a fine
// highlight, the new accent glowing just inside, a faint shadow cast on the old look ahead of it. Chrome runs a polished highlight along the
// lip, starfall lets a few stars glint on it. The old look is drawn once into memory before the change and laid over the new one past the front.
#include <cmath>
#include <cstdint>

#include "bar.h"

namespace {

constexpr float kRevealSec = 0.8f; // as long as astraeus' panel takes to drop
enum Style { kPlain, kGlass, kChrome, kStars };
constexpr float kBulge = 0.30f; // how far the front leads where the pointer was, a share of the screen's depth
constexpr float kBulgeWidth = 0.22f; // and how wide that lead is, a share of its length
constexpr float kWobble = 10, kWobbleHz = 2.2f; // the last small settle as it lands, dips and times a second
constexpr float kStep = 12; // dips between points along the front
constexpr int kGlints = 9; // starfall: stars along the lip
constexpr float kPi = 3.14159265f;

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }

float Hash(uint32_t i) { // a steady scatter: the same stars every frame, only their twinkle moves
    uint32_t x = i * 2654435761u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (float)(x & 0xFFFF) / 65535.0f;
}

ComPtr<ID2D1PathGeometry> Path(const std::vector<D2D1_POINT_2F>& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (pts.size() < 2 || FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, (UINT32)(pts.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

}

void Bar::RevealApply() {
    revealStage = 0;
    if (!revealSnap) { // nothing to pour over: the plain crossfade, as it always was
        SetShell(revealTo);
        if (coverTint) SetMedia(Make(mediaSeed));
        return;
    }
    SetShell(revealTo);
    if (coverTint) SetMedia(Make(mediaSeed));
    shellNow = shellFrom = shellTo, shellT = 1; // the new look whole at once: the pour is the change
    mediaNow = mediaFrom = mediaTo, mediaT = 1;
    revealStyle = revealTo.stars ? kStars : revealTo.chrome ? kChrome : revealTo.glass ? kGlass : kPlain;
    POINT p{};
    bool at = GetCursorPos(&p) && ScreenToClient(hwnd, &p);
    revealAt = {p.x / scale, p.y / scale};
    if (!at || revealAt.x < 0 || revealAt.y < 0 || revealAt.x > W || revealAt.y > winH) revealAt = {(wegR.left + wegR.right) / 2, (wegR.top + wegR.bottom) / 2}; // off the screen: from the dock's middle
    revealT = 0;
}

bool Bar::RevealStep(float dt) {
    if (revealStage != 0 || revealT >= 1 || !revealSnap) {
        if (revealStage == 0) revealSnap.Reset(); // done: its memory goes
        revealOld.Reset(), revealNew.Reset();
        return false;
    }
    revealT = (std::min)(1.0f, revealT + dt / kRevealSec);
    float t = revealT, e = EaseInOut(t), secs = t * kRevealSec;
    bool side = Vert(); // the front pours from the dock's edge: the bottom, or the side it's stuck to
    float along = side ? winH : W, deep = side ? W : winH;
    auto at = [&](float a, float d) {
        if (!side) return D2D1_POINT_2F{a, winH - d};
        return cfg.edge == 1 ? D2D1_POINT_2F{d, a} : D2D1_POINT_2F{W - d, a};
    };
    float hit = side ? revealAt.y : revealAt.x;
    float ahead = kBulge * deep * (1 - e), wide = kBulgeWidth * along; // how far the bulge leads, shrinking as it lands
    float settle = kWobble * std::sin(2 * kPi * kWobbleHz * secs) * std::pow(1 - t, 2.0f);
    float base = -ahead + (deep + ahead + kWobble * 2) * e;
    revealFront.clear();
    for (float a = 0;; a += kStep) {
        float x = (std::min)(a, along), g = (x - hit) / wide;
        revealFront.push_back(at(x, base + ahead * std::exp(-g * g) + settle * std::cos(x / along * kPi * 2)));
        if (x >= along) break;
    }
    std::vector<D2D1_POINT_2F> fresh = revealFront, old = revealFront; // the new look this side of the front, the old one beyond it
    fresh.push_back(at(along, -kWobble * 4)), fresh.push_back(at(0, -kWobble * 4));
    old.push_back(at(along, deep + kWobble * 4)), old.push_back(at(0, deep + kWobble * 4));
    revealNew = Path(fresh, true);
    revealOld = Path(old, true);
    return true;
}

void Bar::RevealOver() {
    if (revealOld) {
        ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), revealOld.Get()), nullptr);
        ctx->DrawBitmap(revealSnap.Get(), D2D1::RectF(0, 0, W, winH));
        ctx->PopLayer();
    }
    float t = revealT, shine = std::sin(kPi * t);
    bool side = Vert();
    D2D1_POINT_2F out = !side ? D2D1_POINT_2F{0, -1} : cfg.edge == 1 ? D2D1_POINT_2F{1, 0} : D2D1_POINT_2F{-1, 0}; // the way the front travels
    auto lip = [&](float off, Color col, float width) {
        std::vector<D2D1_POINT_2F> p = revealFront;
        for (auto& q : p) q.x += out.x * off, q.y += out.y * off;
        if (ComPtr<ID2D1PathGeometry> g = Path(p, false)) canvas->StrokeGeo(g.Get(), col, width);
    };
    const Theme& th = shellNow;
    lip(7, Color{0, 0, 0, 0.16f * shine}, 12); // cast on the old look ahead
    lip(-6, th.accent.Alpha(0.20f * shine), 12); // the new accent, lit from within
    lip(-1, Color{1, 1, 1, (revealStyle == kChrome ? 0.75f : 0.5f) * shine}, 1.4f); // the highlight on the lip
    if (revealStyle == kChrome) { // a polished highlight running along the lip
        float along = side ? winH : W, run = std::fmod(t * 1.6f, 1.0f) * along, half = along * 0.06f;
        std::vector<D2D1_POINT_2F> seg;
        for (const auto& q : revealFront)
            if (std::fabs((side ? q.y : q.x) - run) < half) seg.push_back(q);
        if (ComPtr<ID2D1PathGeometry> g = Path(seg, false)) canvas->StrokeGeo(g.Get(), Color{1, 1, 1, 0.9f * shine}, 2.5f);
    } else if (revealStyle == kStars) { // a few stars riding the lip
        for (uint32_t i = 0; i < (uint32_t)kGlints && !revealFront.empty(); ++i) {
            const D2D1_POINT_2F& q = revealFront[(size_t)(Hash(i) * (revealFront.size() - 1))];
            float tw = 0.5f + 0.5f * std::sin(t * 16 + (float)i * 2.3f);
            canvas->Dot(q, 4.5f, th.accentHi.Alpha(0.18f * shine * tw));
            canvas->Dot(q, 1.4f, Color{1, 1, 1, 0.9f * shine * tw});
        }
    }
}
