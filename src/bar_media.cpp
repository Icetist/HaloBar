#include "bar.h"

#include <cmath>

#include "glyphs.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kBlobs = 5; // a few big soft blobs across the player, as on island's media card

std::wstring Clock(double seconds) {
    int s = (int)(std::max)(0.0, seconds);
    wchar_t buf[16];
    if (s >= 3600) swprintf_s(buf, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}

std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

void WaveLine(Canvas& c, float a0, float a1, float cy, float amp, double now, Color col, float width, float wavelen) {
    if (a1 <= a0) return;
    if (amp < 0.05f) { // flat: plain line, skip geometry
        c.Line({a0, cy}, {a1, cy}, col, width);
        return;
    }
    const float taper = 16; // edge taper px
    float phase = (float)std::fmod(now * 2 * kPi * 0.75, 2 * kPi); // 0.75 hz scroll, wrapped
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    auto yAt = [&](float px) {
        float k = (std::min)(1.0f, (std::min)((px - a0) / taper, (a1 - px) / taper)); // ramp amp to 0 at both ends
        return cy + amp * (std::max)(0.0f, k) * std::sin(2 * kPi * px / wavelen - phase);
    };
    sink->BeginFigure({a0, yAt(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (float px = a0 + 2; px < a1; px += 2) sink->AddLine({px, yAt(px)}); // 2px polyline steps
    sink->AddLine({a1, yAt(a1)});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    c.StrokeGeo(path.Get(), col, width);
}

}

void Bar::OnMedia() {
    sys::Track t;
    bool has = media.Snapshot(t);
    bool started = has && t.playing && !(hasTrack && track.playing);
    track = std::move(t);
    hasTrack = has;
    if (started) meter.Refresh(); // follow whichever output it's playing on now
    if (!hasTrack && mediaOpen) SetMediaOpen(false);
    if (card.Shown()) ShowPreview(); // a card with the transport on it shows play or pause as it is now
    if (hasTrack && track.music && track.duration > 0 && !track.title.empty()) { // looked up while it plays, so the player has them the moment it opens
        std::wstring k = sys::LyricSrc::Key(track.title, track.artist);
        if (k != lyrKey) lyrKey = k, lyrLine = -2;
        lyrics.Want(track.title, track.artist, track.album, track.duration / 1e7);
    }
    Redraw();
}

void Bar::SyncCover() {
    uint64_t id = hasTrack ? track.coverId : 0;
    if (id != coverId) { // a new song's art: decode once, colors from it
        coverId = id;
        cover.Reset();
        coverBlur.Reset();
        coverSrc.Reset();
        if (hasTrack && track.cover && !track.cover->empty()) coverSrc = gfx::Decode(track.cover->data(), track.cover->size(), 512);
        if (coverSrc) SetMedia(Make(mediaSeed = theme::FromBmp(coverSrc.Get())));
        else SetMedia(shellTo);
    }
    if (coverSrc && !cover && ctx && SUCCEEDED(ctx->CreateBitmapFromWicBitmap(coverSrc.Get(), nullptr, &cover)))
        coverBlur = gfx::BakeBlur(ctx.Get(), cover.Get(), 26.0f); // heavy blur for the player's backdrop, baked once
}

bool Bar::StepBars(float dt, double now) {
    bool playing = hasTrack && track.playing;
    float l = 0, r = 0;
    if (playing && !meter.Read(l, r)) meter.Refresh(); // the output changed under us: listen to the new one
    float level = std::sqrt((l + r) / 2); // sqrt lifts quiet passages so the bars stay alive
    bool moving = false;
    for (int i = 0; i < 4; ++i) {
        float wobble = 0.55f + 0.45f * std::sin((float)now * (5.0f + i * 1.7f) + i * 1.3f); // each bar on its own rhythm
        float want = playing ? anim::Clamp01(level * wobble * 1.4f) : 0;
        float& v = bars[i];
        v += (want - v) * (std::min)(1.0f, dt * 14); // quick attack, smooth fall
        moving = moving || std::fabs(want - v) > 0.01f || want > 0.01f;
    }
    bool seen = sink < 0.98f && !full && mediaShow > 0.02f; // tucked away or behind a fullscreen app: no one sees them, so no frames at all
    return seen && (moving || playing); // a quiet moment must not stop the frames, or the bars stay flat once the music is back
}

void Bar::DrawMediaRow(const Theme& t) {
    barsMoving = false;
    if (mediaShow < 0.02f) return;
    Canvas& c = ui.C();
    const Theme& mt = mediaNow; // the song's own colors
    float a = anim::Clamp01((mediaShow - 0.4f) / 0.6f);
    uint32_t id = Ui::Id("media.row");
    Ui::Hit h = ui.Button(id, mediaR);
    if (h.clicked) SetMediaOpen(!mediaOpen);
    float hov = ui.Smooth(id + 1, h.hover && !mediaOpen ? 1.0f : 0, 14);
    if (hov > 0.01f) c.Fill(Inset(mediaR, -4, -1), Height(mediaR) / 2 + 1, t.text.Alpha(0.09f * hov));
    if (a <= 0) return;
    float cy = rowTop + kRowH / 2;
    c.PushClip(Inset(mediaR, -2, -4));
    D2D1_RECT_F art = Rect(mediaR.left + 2, cy - kArt / 2, kArt, kArt);
    if (cover) {
        c.Art(cover.Get(), coverBlur.Get(), art, kArt * 0.25f, a); // island's art radius: a quarter of its size
    } else {
        c.Fill(art, kArt * 0.25f, mt.cardHi.Alpha(a));
        c.Icon(glyph::Music, {art.left + kArt / 2, art.top + kArt / 2}, 15, mt.accentHi.Alpha(a), true);
    }
    float bx = mediaR.right - kBarsW;
    float tx = art.right + 8, tw = bx - 10 - tx;
    c.Text(track.title, Rect(tx, cy - 15, tw, 16), Weight::Medium, 12, t.text.Alpha(a));
    if (!track.artist.empty()) c.Text(track.artist, Rect(tx, cy + 1, tw, 14), Weight::Light, 10.5f, t.textDim.Alpha(a));
    c.PopClip();
    barsRect = Rect(bx, cy - 9, kBarsW, 18);
    DrawBars(ui.Dt(), ui.Now());
}

void Bar::DrawBars(float dt, double now, bool step) { // the row's mini visualizer, as island's pill; a frame where only these move draws nothing else
    if (step) barsMoving = StepBars(dt, now);
    float a = anim::Clamp01((mediaShow - 0.4f) / 0.6f), cy = (barsRect.top + barsRect.bottom) / 2;
    if (a <= 0) return;
    const Theme& mt = mediaNow; // the song's own colors
    for (int i = 0; i < 4; ++i) {
        float v = bars[i], bh = 3 + 11 * v; // island's pill bars
        barsDrawn[i] = v;
        canvas->Fill(Rect(barsRect.left + i * 6, cy - bh / 2, 3.5f, bh), 1.75f, mt.accent.Mix(mt.accentHi, v).Alpha(a));
    }
}

void Bar::Wave(const D2D1_RECT_F& r, const Theme& t) {
    Canvas& c = ui.C();
    uint32_t id = Ui::Id("mp.seek");
    bool known = track.duration > 0; // some apps (firefox-based browsers) never report length or position
    float v = !known ? 1.0f : preview >= 0 ? preview : (float)track.Progress();
    if (known && track.canSeek && ui.Slider(id, r, v, false)) preview = v; // preview seek while dragging
    if (ui.IsActive(id) && ui.In().released) { // commit seek on release only
        media.Seek(v);
        preview = -1;
    }
    if (!ui.IsActive(id)) preview = -1;
    float amp = ui.Smooth(id + 1, track.playing && !ui.IsActive(id) ? 3.2f : 0, 5); // the wave flattens when paused or dragging
    float cy = (r.top + r.bottom) / 2;
    float x = r.left + Width(r) * anim::Clamp01(v);
    const float gap = 7; // gap around the head
    float a0 = r.left + 2, a1 = known ? x - gap : r.right - 2;
    WaveLine(c, a0, a1, cy, amp, ui.Now(), t.accent, 3.5f, 28);
    if (amp >= 0.05f && a1 > a0) ui.KeepAlive();
    if (!known) return;
    if (x + gap < r.right - 2) c.Line({x + gap, cy}, {r.right - 2, cy}, t.accent.Alpha(0.3f), 3.5f);
    float grow = ui.Smooth(id + 2, ui.IsActive(id) || Inside(r, ui.In().mouse) ? 1.0f : 0, 16); // head grows on hover/drag
    c.Fill({x - 2, cy - 9 - 3 * grow, x + 2, cy + 9 + 3 * grow}, 2, t.accentHi);
}

bool Bar::StepSwell(float dt, bool playing) {
    float raw[sys::Spectrum::kBands] = {};
    if (playing) { // paused: the light sinks away and the analyser stops by itself
        spectrumAsked = ui.Now();
        if (!spectrum) spectrum = std::make_unique<sys::Spectrum>();
        spectrum->Read(raw);
    }
    const int first = 2, span = 52; // ~40 Hz to ~10 kHz, where music actually lives
    float sum = 0;
    bool alive = false;
    for (int i = 0; i < kBlobs; ++i) {
        int b0 = first + i * span / kBlobs, b1 = (std::max)(b0 + 1, first + (i + 1) * span / kBlobs);
        float want = 0;
        for (int b = b0; b < b1; ++b) want += raw[b];
        want /= (b1 - b0); // average: blobs swell, they don't spike
        float& g = swell.gain[i];
        g = (std::max)(want, g - dt * 0.12f); // its recent loudest, slowly forgotten: every blob uses its whole range at any volume
        want = anim::Clamp01((want / (std::max)(g, 0.18f) - 0.30f) / 0.70f); // relative to that: quiet sinks, every hit stands up
        float& v = swell.level[i];
        v += (want - v) * (std::min)(1.0f, dt * (want > v ? 16.0f : 3.2f)); // swells fast, sinks soon after
        sum += v;
        alive = alive || v > 0.005f;
    }
    swell.heat += (sum / kBlobs - swell.heat) * (std::min)(1.0f, dt * 2.0f);
    return alive || swell.heat > 0.003f;
}

void Bar::DrawSwell(const D2D1_RECT_F& r, const Theme& t, double now) {
    Canvas& c = ui.C();
    float w = Width(r), h = Height(r), slot = w / kBlobs, tm = (float)now;
    if (swell.heat > 0.003f) c.Glow({(r.left + r.right) / 2, r.bottom}, w * 0.70f, h * (0.25f + 0.45f * swell.heat), t.accent.Alpha(0.12f + 0.33f * swell.heat)); // warm wash along the floor
    for (int i = 0; i < kBlobs; ++i) {
        float v = swell.level[i];
        if (v < 0.01f) continue;
        float drift = std::sin(tm * 0.22f + i * 2.3f) * slot * 0.35f; // each blob wanders slowly along the floor
        float x = r.left + slot * (i + 0.5f) + drift, base = r.bottom + h * 0.06f; // sits just below the edge, so only its top shows
        float rise = h * (0.35f + 1.05f * v);
        c.Glow({x, base}, slot * 1.6f * (0.85f + 0.35f * v), rise, t.accent.Alpha(0.13f + 0.52f * v)); // big and soft, melting into its neighbours
        c.Glow({x, base}, slot * 0.85f, rise * 0.70f, t.accentHi.Alpha(0.36f * v)); // a brighter heart
    }
}

void Bar::DrawPlayer(ID2D1Geometry* shape) {
    float grown = (std::max)(0.0f, mediaSpring.value);
    if (grown < 1 || !shape) return;
    Canvas& c = ui.C();
    const Theme& t = mediaNow;
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((grown - 40) / (kMediaRise - 40));

    // the whole island takes on the song: its blurred cover and the music's light, kept inside the shape
    rt->PushLayer(D2D1::LayerParameters(pill, shape), nullptr);
    if (coverBlur) {
        c.Image(coverBlur.Get(), pill, 0, 0.6f * a);
        c.FillV(pill, 0, t.veil.Alpha(0.58f * a), t.veil.Alpha(0.92f * a)); // darker toward the row, so it stays legible even over a white cover
    }
    if (StepSwell(ui.Dt(), hasTrack && track.playing)) {
        ui.KeepAlive(); // animate until fully faded
        DrawSwell(Rect(pill.left, pill.top, Width(pill), rowTop - pill.top), t, ui.Now());
    }
    rt->PopLayer();
    if (a <= 0.01f) return;

    // widgets draw at full ink: fade them as one, clipped to the island while it grows
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    float px = W / 2 - kMediaW / 2, y0 = pill.top + CallH() + 22; // laid out at full width, so nothing slides as it opens
    D2D1_RECT_F art = Rect(px + 24, y0, 134, 134);
    c.Glow({(art.left + art.right) / 2, (art.top + art.bottom) / 2 + 8}, 84, 84, Color{0, 0, 0, 0.35f}); // a soft shadow under the art
    c.Fill(art, 20, t.cardHi);
    if (cover) c.Art(cover.Get(), coverBlur.Get(), art, 20);
    else c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, 52, t.textLow);

    float x = art.right + 22, w = 300; // song and controls; the lyrics take the rest
    D2D1_RECT_F lyr = {x + w + 28, y0 - 4, px + kMediaW - 26, y0 + 150};
    c.Line({lyr.left - 14, lyr.top + 10}, {lyr.left - 14, lyr.bottom - 10}, t.line, 1);
    DrawLyrics(lyr, t);
    c.TextSp(Upper(track.app), Rect(x, y0, w, 14), Weight::Medium, 10.5f, t.accent, 1.6f);
    c.Text(track.title.empty() ? L"Nothing playing" : track.title, Rect(x, y0 + 16, w, 32), Weight::Medium, 24, t.text);
    std::wstring sub = track.album.empty() || track.album == track.artist ? track.artist : track.artist + L"  ·  " + track.album;
    c.Text(sub, Rect(x, y0 + 48, w, 20), Weight::Light, 14, t.textDim);

    float ww = (std::min)(w, 300.0f);
    D2D1_RECT_F seek = Rect(x, y0 + 72, ww, 22);
    Wave(seek, t);
    if (track.duration > 0) {
        double total = track.duration / 1e7, elapsed = preview >= 0 ? total * preview : track.Elapsed() / 1e7; // 100ns units
        c.Text(Clock(elapsed), Rect(x, seek.bottom - 2, 70, 14), Weight::Regular, 10.5f, t.textDim);
        c.Text(Clock(total), Rect(seek.right - 70, seek.bottom - 2, 70, 14), Weight::Regular, 10.5f, t.textDim, Align::Right);
    }

    float cy = y0 + 126, cx = x + ww / 2;
    const Theme ct = theme::Lift(t); // the play button leans toward the ink, so it stands clear of the glow behind it
    if (w::IconBtn(ui, ct, Ui::Id("mp.prev"), {cx - 58, cy}, 40, glyph::Prev, 26, track.canPrev)) media.Previous();
    if (w::PlayBtn(ui, ct, Ui::Id("mp.play"), {cx, cy}, 48, track.playing, hasTrack)) media.TogglePlay();
    if (w::IconBtn(ui, ct, Ui::Id("mp.next"), {cx + 58, cy}, 40, glyph::Next, 26, track.canNext)) media.Next();
    Theme lit = t; // shuffle and repeat light up in the song's own accent, not the lifted one the transport uses
    lit.accent = t.accentHi;
    if (w::IconBtn(ui, lit, Ui::Id("mp.shuf"), {cx - 108, cy}, 34, glyph::Shuffle, 19, track.canShuf, track.shuffle)) media.ToggleShuffle();
    if (w::IconBtn(ui, lit, Ui::Id("mp.rep"), {cx + 108, cy}, 34, track.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 19, track.canRep, track.repeat != 0))
        media.CycleRepeat();
    if (track.shuffle) c.Dot({cx - 108, cy + 15}, 2.2f, t.accentHi); // on, as island marks it
    if (track.repeat) c.Dot({cx + 108, cy + 15}, 2.2f, t.accentHi);
    rt->PopLayer();
}

void Bar::DrawLyrics(const D2D1_RECT_F& r, const Theme& t) {
    Canvas& c = ui.C();
    std::shared_ptr<const sys::Lyrics> got = lyrKey.empty() ? nullptr : lyrics.Get(lyrKey);
    if (!got || !got->done) { // still looking: check back now and then, without keeping frames running
        if (!lyrKey.empty()) SetTimer(hwnd, kLyricTimer, 500, nullptr);
        float cy = (r.top + r.bottom) / 2;
        c.Text(lyrKey.empty() ? L"No lyrics for this one" : L"Finding lyrics\u2026", Rect(r.left, cy - 9, Width(r), 18), Weight::Light, 13, t.textLow, Align::Center);
        return;
    }
    if (got->interim) SetTimer(hwnd, kLyricTimer, 1500, nullptr); // a word-timed version may still replace it
    const auto& lines = got->lines;
    if (lines.empty()) {
        float cy = (r.top + r.bottom) / 2;
        c.Text(L"No synced lyrics for this song", Rect(r.left, cy - 9, Width(r), 18), Weight::Light, 13, t.textLow, Align::Center);
        return;
    }
    constexpr float kRest = 13.5f; // lines around the sung one
    if (got.get() != lyrDoc) { // new lyrics: measure every line once, shrink the long ones to fit
        lyrDoc = got.get();
        lyrFit.assign(lines.size(), kRest);
        for (size_t i = 0; i < lines.size(); ++i) {
            float w = TextW(lines[i].text, Weight::Light, kRest);
            if (w > Width(r)) lyrFit[i] = (std::max)(9.5f, kRest * Width(r) / w);
        }
    }
    double now = track.Elapsed() / 1e7 + got->offsetMs / 1000.0;
    int cur = -1;
    for (int i = 0; i < (int)lines.size() && lines[i].start <= now; ++i) cur = i;
    constexpr float kLineH = 30, kSung = 16;
    if (cur != lyrLine) { // a new line: laid out once, on one row, scaled down to fit the column if it's long
        lyrLine = cur;
        lyrLay.Reset();
        if (cur >= 0 && !lines[cur].text.empty()) {
            const std::wstring& tx = lines[cur].text;
            float size = kSung;
            for (int attempt = 0; attempt < 2; ++attempt) {
                lyrLay.Reset();
                if (FAILED(gfx::DWrite()->CreateTextLayout(tx.c_str(), (UINT32)tx.size(), gfx::TextFmt(Weight::Medium, size, Align::Left), 100000, kLineH, &lyrLay))) break;
                lyrLay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TEXT_METRICS m{};
                lyrLay->GetMetrics(&m);
                if (m.width <= Width(r) || attempt == 1) break;
                size = (std::max)(11.0f, size * Width(r) / m.width); // too wide: the same line, a little smaller
            }
        }
    }
    float at = ui.Smooth(Ui::Id("lyr.scroll"), (float)(std::max)(cur, 0), 9); // glides to the next line
    if (track.playing) ui.KeepAlive();
    float cy = (r.top + r.bottom) / 2;
    ID2D1RenderTarget* rt = c.Target();
    c.PushClip(r);
    for (int i = (std::max)(0, cur - 3); i < (int)lines.size() && i <= cur + 4; ++i) {
        float y = cy + (i - at) * kLineH;
        float d = std::fabs(i - at), fade = anim::Clamp01(1 - d / 3.2f);
        if (fade <= 0.02f || lines[i].text.empty()) continue;
        D2D1_RECT_F lr = Rect(r.left, y - kLineH / 2, Width(r), kLineH);
        if (i != cur || !lyrLay) {
            c.Text(lines[i].text, lr, Weight::Light, i < (int)lyrFit.size() ? lyrFit[i] : kRest, t.textDim.Alpha(0.85f * fade), Align::Left);
            continue;
        }
        auto xAt = [&](int ch) { // where a letter starts on the laid-out line, exactly
            float x = 0, yy = 0;
            DWRITE_HIT_TEST_METRICS hm{};
            if (ch >= (int)lines[i].text.size()) {
                DWRITE_TEXT_METRICS m{};
                lyrLay->GetMetrics(&m);
                return m.width;
            }
            lyrLay->HitTestTextPosition((UINT32)(std::max)(0, ch), FALSE, &x, &yy, &hm);
            return x;
        };
        float fill;
        if (!lines[i].words.empty()) { // word-timed: up to the word being sung, through it as it's sung
            const auto& ws = lines[i].words;
            size_t k = 0;
            while (k + 1 < ws.size() && ws[k + 1].first <= now) ++k;
            double ws0 = ws[k].first, ws1 = k + 1 < ws.size() ? ws[k + 1].first : lines[i].end;
            int o1 = k + 1 < ws.size() ? ws[k + 1].second : (int)lines[i].text.size();
            float in = ws1 > ws0 ? (float)std::clamp((now - ws0) / (ws1 - ws0), 0.0, 1.0) : 1;
            float x0 = xAt(ws[k].second), x1 = xAt(o1);
            fill = now < ws0 ? 0 : x0 + (x1 - x0) * in;
        } else {
            double span = (std::max)(0.3, lines[i].end - lines[i].start);
            fill = (float)std::clamp((now - lines[i].start) / span, 0.0, 1.0) * xAt((int)lines[i].text.size());
        }
        DWRITE_TEXT_METRICS m{};
        lyrLay->GetMetrics(&m);
        D2D1_POINT_2F org = {lr.left, lr.top + (kLineH - m.height) / 2};
        ComPtr<ID2D1SolidColorBrush> wait, sung;
        rt->CreateSolidColorBrush(t.text.Alpha(0.45f).D2D(), &wait);
        rt->CreateSolidColorBrush(t.accentHi.Mix(t.text, 0.35f).D2D(), &sung);
        if (wait) rt->DrawTextLayout(org, lyrLay.Get(), wait.Get()); // the line, waiting
        if (sung && fill > 0) {
            c.PushClip({lr.left, lr.top, lr.left + fill, lr.bottom});
            rt->DrawTextLayout(org, lyrLay.Get(), sung.Get()); // and sung, filling in word by word
            c.PopClip();
        }
    }
    c.PopClip();
}
