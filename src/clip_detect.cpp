// What a copy is: a link, a colour in any of its notations, code and its language, or plain words; and the small helpers
// the clipboard panel shows them with: colour formats, code colours, fuzzy search, "5m ago".
#include "clips.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_set>

namespace clip {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

bool Starts(const std::wstring& s, const wchar_t* p) { return s.compare(0, wcslen(p), p) == 0; }

bool Has(const std::wstring& s, const wchar_t* p) { return s.find(p) != std::wstring::npos; }

// ---- colours ----

double Gamma(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
double Linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double Clamp(double v, double lo = 0, double hi = 1) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t Pack(double r, double g, double b, double a) { // srgb 0..1 to 0xAARRGGBB
    auto ch = [](double v) { return (uint32_t)std::lround(Clamp(v) * 255); };
    return ch(a) << 24 | ch(r) << 16 | ch(g) << 8 | ch(b);
}

void Hsl(double h, double s, double l, double& r, double& g, double& b) {
    h = std::fmod(std::fmod(h, 360) + 360, 360) / 360;
    auto f = [&](double n) {
        double k = std::fmod(n + h * 12, 12), a = s * (std::min)(l, 1 - l);
        return l - a * (std::max)(-1.0, (std::min)({k - 3, 9 - k, 1.0}));
    };
    r = f(0), g = f(8), b = f(4);
}

void Hsv(double h, double s, double v, double& r, double& g, double& b) {
    double l = v * (1 - s / 2), sl = (l == 0 || l == 1) ? 0 : (v - l) / (std::min)(l, 1 - l);
    Hsl(h, sl, l, r, g, b);
}

void FromXyzD50(double x, double y, double z, double& r, double& g, double& b) { // bradford to d65, then linear srgb
    double X = 0.9554734527042182 * x - 0.023098536874261423 * y + 0.0632593086610217 * z;
    double Y = -0.028369706963208136 * x + 1.0099954580058226 * y + 0.021041398966943008 * z;
    double Z = 0.012314001688319899 * x - 0.020507696433477912 * y + 1.3303659366080753 * z;
    r = Gamma(3.2409699419045226 * X - 1.537383177570094 * Y - 0.4986107602930034 * Z);
    g = Gamma(-0.9692436362808796 * X + 1.8759675015077202 * Y + 0.04155505740717559 * Z);
    b = Gamma(0.05563007969699366 * X - 0.20397695888897652 * Y + 1.0569715142428786 * Z);
}

void Lab(double L, double a, double bb, double& r, double& g, double& b) { // cie lab, d50 as css has it
    constexpr double kE = 216.0 / 24389, kK = 24389.0 / 27;
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - bb / 200;
    auto inv = [&](double t) { return t * t * t > kE ? t * t * t : (116 * t - 16) / kK; };
    double x = inv(fx) * 0.96422, y = (L > kK * kE ? fy * fy * fy : L / kK), z = inv(fz) * 0.82521;
    FromXyzD50(x, y, z, r, g, b);
}

void Oklab(double L, double a, double bb, double& r, double& g, double& b) {
    double l = L + 0.3963377774 * a + 0.2158037573 * bb, m = L - 0.1055613458 * a - 0.0638541728 * bb, s = L - 0.0894841775 * a - 1.2914855480 * bb;
    l = l * l * l, m = m * m * m, s = s * s * s;
    r = Gamma(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    g = Gamma(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    b = Gamma(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

struct Num {
    double v = 0;
    wchar_t unit = 0; // '%', 'd' degrees, 'r' radians, 't' turns
};

bool Numbers(const std::wstring& in, std::vector<Num>& out) { // "12, 50% 0.3 / 90deg": separators are spaces, commas and slashes
    out.clear();
    size_t i = 0;
    while (i < in.size()) {
        wchar_t c = in[i];
        if (c == L' ' || c == L',' || c == L'/' || c == L'\t') {
            ++i;
            continue;
        }
        wchar_t* end = nullptr;
        double v = wcstod(in.c_str() + i, &end);
        size_t j = (size_t)(end - in.c_str());
        if (j == i) return false;
        Num n{v, 0};
        std::wstring unit;
        while (j < in.size() && (iswalpha(in[j]) || in[j] == L'%')) unit += (wchar_t)towlower(in[j++]);
        if (unit == L"%") n.unit = L'%';
        else if (unit == L"deg") n.unit = L'd';
        else if (unit == L"rad") n.unit = L'r';
        else if (unit == L"turn") n.unit = L't';
        else if (!unit.empty()) return false;
        out.push_back(n);
        i = j;
    }
    return !out.empty();
}

double Hue(const Num& n) { return n.unit == L'r' ? n.v * 180 / kPi : n.unit == L't' ? n.v * 360 : n.v; }
double Frac(const Num& n, double whole) { return n.unit == L'%' ? n.v / 100 : n.v / whole; } // a percentage, or a plain number out of whole
double Alpha(const std::vector<Num>& v, size_t at) { return v.size() > at ? Clamp(v[at].unit == L'%' ? v[at].v / 100 : v[at].v) : 1; }

bool Labeled(const std::wstring& s, const wchar_t* name, double& v) { // swift's "red: 0.5" or "red: 128/255"
    std::wstring low = Lower(s);
    size_t at = low.find(name);
    if (at == std::wstring::npos) return false;
    const wchar_t* p = low.c_str() + at + wcslen(name);
    wchar_t* end = nullptr;
    v = wcstod(p, &end);
    if (end == p) return false;
    while (*end == L' ') ++end;
    if (*end == L'/') v /= (std::max)(1e-9, wcstod(end + 1, nullptr));
    return true;
}

bool Hex(const std::wstring& h, uint32_t& argb, bool argbOrder) { // 3, 4, 6 or 8 digits
    for (wchar_t c : h)
        if (!iswxdigit(c)) return false;
    uint32_t v = (uint32_t)wcstoul(h.c_str(), nullptr, 16);
    auto twice = [](uint32_t n) { return n << 4 | n; };
    switch (h.size()) {
    case 3: argb = 0xFF000000 | twice(v >> 8 & 15) << 16 | twice(v >> 4 & 15) << 8 | twice(v & 15); return true;
    case 4: argb = twice(v & 15) << 24 | twice(v >> 12 & 15) << 16 | twice(v >> 8 & 15) << 8 | twice(v >> 4 & 15); return true;
    case 6: argb = 0xFF000000 | v; return true;
    case 8: argb = argbOrder ? v : (v & 0xFF) << 24 | v >> 8; return true; // css: rrggbbaa; 0x: aarrggbb, as android and .net write it
    default: return false;
    }
}

}

bool ParseColor(const std::wstring& raw, uint32_t& argb) {
    std::wstring s = raw;
    while (!s.empty() && (s.back() == L';' || s.back() == L' ')) s.pop_back();
    if (s.size() < 4 || s.size() > 120 || s.find(L'\n') != std::wstring::npos) return false;
    if (s[0] == L'#') return Hex(s.substr(1), argb, false);
    if ((s[0] == L'0') && (s[1] == L'x' || s[1] == L'X') && (s.size() == 8 || s.size() == 10)) return Hex(s.substr(2), argb, true);
    size_t open = s.find(L'('), close = s.rfind(L')');
    if (open == std::wstring::npos || close != s.size() - 1 || open == 0) return false;
    std::wstring fn = Lower(s.substr(0, open)), in = s.substr(open + 1, close - open - 1);
    while (!fn.empty() && fn.back() == L' ') fn.pop_back();
    double r = 0, g = 0, b = 0, a = 1;
    if (fn == L"uicolor" || fn == L"nscolor" || fn == L"color" || fn == L"cgcolor" || fn == L"uicolor.init" || fn == L"nscolor.init") { // swift's labeled forms
        double h = 0, sat = 0, br = 0;
        if (Labeled(in, L"red:", r) && Labeled(in, L"green:", g) && Labeled(in, L"blue:", b)) {
        } else if (Labeled(in, L"hue:", h) && Labeled(in, L"saturation:", sat) && Labeled(in, L"brightness:", br)) {
            Hsv(h * 360, sat, br, r, g, b);
        } else if (fn == L"color" && Starts(Lower(in), L"srgb ")) { // css color(srgb r g b)
            std::vector<Num> v;
            if (!Numbers(in.substr(5), v) || v.size() < 3) return false;
            r = Frac(v[0], 1), g = Frac(v[1], 1), b = Frac(v[2], 1), a = Alpha(v, 3);
            argb = Pack(r, g, b, a);
            return true;
        } else {
            return false;
        }
        if (!Labeled(in, L"alpha:", a) && !Labeled(in, L"opacity:", a)) a = 1;
        if (r > 1 || g > 1 || b > 1) r /= 255, g /= 255, b /= 255; // 0-255 written straight
        argb = Pack(r, g, b, a);
        return true;
    }
    std::vector<Num> v;
    if (fn == L"color") { // css color(display-p3 ...), read as srgb: close enough for a swatch
        size_t sp = in.find(L' ');
        if (sp == std::wstring::npos) return false;
        std::wstring space = Lower(in.substr(0, sp));
        if (!Numbers(in.substr(sp + 1), v) || v.size() < 3) return false;
        r = Frac(v[0], 1), g = Frac(v[1], 1), b = Frac(v[2], 1);
        if (space == L"srgb-linear") r = Gamma(r), g = Gamma(g), b = Gamma(b);
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if (!Numbers(in, v)) return false;
    size_t n = v.size();
    if ((fn == L"rgb" || fn == L"rgba") && n >= 3) {
        argb = Pack(Frac(v[0], 255), Frac(v[1], 255), Frac(v[2], 255), Alpha(v, 3));
        return true;
    }
    if ((fn == L"hsl" || fn == L"hsla") && n >= 3) {
        Hsl(Hue(v[0]), Frac(v[1], 100), Frac(v[2], 100), r, g, b);
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if ((fn == L"hsv" || fn == L"hsb") && n >= 3) {
        Hsv(Hue(v[0]), Frac(v[1], 100), Frac(v[2], 100), r, g, b);
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if (fn == L"hwb" && n >= 3) {
        double w = Frac(v[1], 100), bl = Frac(v[2], 100);
        if (w + bl >= 1) r = g = b = w / (w + bl);
        else {
            Hsl(Hue(v[0]), 1, 0.5, r, g, b);
            r = r * (1 - w - bl) + w, g = g * (1 - w - bl) + w, b = b * (1 - w - bl) + w;
        }
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if (fn == L"lab" && n >= 3) {
        Lab(v[0].v, v[1].unit == L'%' ? v[1].v * 1.25 : v[1].v, v[2].unit == L'%' ? v[2].v * 1.25 : v[2].v, r, g, b);
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if (fn == L"lch" && n >= 3) {
        double c = v[1].unit == L'%' ? v[1].v * 1.5 : v[1].v, h = Hue(v[2]) * kPi / 180;
        Lab(v[0].v, c * std::cos(h), c * std::sin(h), r, g, b);
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if ((fn == L"oklab" || fn == L"oklch") && n >= 3) {
        double L = v[0].unit == L'%' ? v[0].v / 100 : v[0].v;
        if (fn == L"oklab") Oklab(L, v[1].unit == L'%' ? v[1].v * 0.004 : v[1].v, v[2].unit == L'%' ? v[2].v * 0.004 : v[2].v, r, g, b);
        else {
            double c = v[1].unit == L'%' ? v[1].v * 0.004 : v[1].v, h = Hue(v[2]) * kPi / 180;
            Oklab(L, c * std::cos(h), c * std::sin(h), r, g, b);
        }
        argb = Pack(r, g, b, Alpha(v, 3));
        return true;
    }
    if ((fn == L"cmyk" || fn == L"device-cmyk") && n >= 4) {
        auto k = [&](const Num& x) { return x.unit == L'%' ? x.v / 100 : x.v > 1 ? x.v / 100 : x.v; };
        argb = Pack((1 - k(v[0])) * (1 - k(v[3])), (1 - k(v[1])) * (1 - k(v[3])), (1 - k(v[2])) * (1 - k(v[3])), Alpha(v, 4));
        return true;
    }
    bool net = fn == L"color.fromargb" || fn == L"color.argb", rgbCall = fn == L"color.rgb" || fn == L"new color" || fn == L"rgbcolor" || fn == L"color.fromrgb";
    if (net && n == 4) { // .net and android: alpha first, 0-255
        argb = Pack(v[1].v / 255, v[2].v / 255, v[3].v / 255, v[0].v / 255);
        return true;
    }
    if ((net || rgbCall) && n >= 3) {
        bool unit = v[0].v <= 1 && v[1].v <= 1 && v[2].v <= 1 && (v[0].v != (int)v[0].v || v[1].v != (int)v[1].v || v[2].v != (int)v[2].v); // java's float form
        double k = unit ? 1 : 255;
        argb = Pack(v[0].v / k, v[1].v / k, v[2].v / k, n > 3 ? v[3].v / k : 1);
        return true;
    }
    return false;
}

std::vector<Fmt> Formats(uint32_t argb) {
    double r = (argb >> 16 & 255) / 255.0, g = (argb >> 8 & 255) / 255.0, b = (argb & 255) / 255.0, a = (argb >> 24) / 255.0;
    wchar_t buf[128];
    std::vector<Fmt> out;
    if (a < 1) swprintf_s(buf, L"#%06X%02X", argb & 0xFFFFFF, argb >> 24);
    else swprintf_s(buf, L"#%06X", argb & 0xFFFFFF);
    out.push_back({L"HEX", buf});
    swprintf_s(buf, a < 1 ? L"rgb(%d %d %d / %.2f)" : L"rgb(%d %d %d)", (int)(argb >> 16 & 255), (int)(argb >> 8 & 255), (int)(argb & 255), a);
    out.push_back({L"RGB", buf});
    double mx = (std::max)({r, g, b}), mn = (std::min)({r, g, b}), l = (mx + mn) / 2, d = mx - mn, h = 0, s = 0;
    if (d > 1e-9) {
        s = d / (1 - std::fabs(2 * l - 1));
        h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h = std::fmod(h * 60 + 360, 360);
    }
    swprintf_s(buf, L"hsl(%.0f %.0f%% %.0f%%)", h, s * 100, l * 100);
    out.push_back({L"HSL", buf});
    double lr = Linear(r), lg = Linear(g), lb = Linear(b);
    double L = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb), M = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb),
           S = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
    double ok = 0.2104542553 * L + 0.7936177850 * M - 0.0040720468 * S, oa = 1.9779984951 * L - 2.4285922050 * M + 0.4505937099 * S,
           ob = 0.0259040371 * L + 0.7827717662 * M - 0.8086757660 * S;
    double oc = std::hypot(oa, ob), oh = std::fmod(std::atan2(ob, oa) * 180 / kPi + 360, 360);
    swprintf_s(buf, L"oklch(%.3f %.3f %.1f)", ok, oc, oc < 1e-4 ? 0.0 : oh);
    out.push_back({L"OKLCH", buf});
    double k = 1 - mx;
    auto cm = [&](double ch) { return k >= 1 ? 0.0 : (1 - ch - k) / (1 - k) * 100; };
    swprintf_s(buf, L"cmyk(%.0f%% %.0f%% %.0f%% %.0f%%)", cm(r), cm(g), cm(b), k * 100);
    out.push_back({L"CMYK", buf});
    swprintf_s(buf, L"Color(red: %.3f, green: %.3f, blue: %.3f)", r, g, b);
    out.push_back({L"SWIFT", buf});
    return out;
}

// ---- links ----

bool IsLink(const std::wstring& s) {
    if (s.size() < 8 || s.size() > 4096 || s.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
    std::wstring l = Lower(s.substr(0, 12));
    return Starts(l, L"http://") || Starts(l, L"https://") || Starts(l, L"www.") || Starts(l, L"ftp://");
}

std::wstring Host(const std::wstring& url) {
    size_t at = url.find(L"://");
    size_t from = at == std::wstring::npos ? 0 : at + 3;
    size_t to = url.find_first_of(L"/?#:", from);
    std::wstring h = Lower(url.substr(from, to == std::wstring::npos ? std::wstring::npos : to - from));
    if (size_t user = h.find(L'@'); user != std::wstring::npos) h = h.substr(user + 1);
    if (Starts(h, L"www.")) h = h.substr(4);
    return h;
}

bool Adult(const std::wstring& host) { // a short list of the big ones and their telltale words; a link or a page there is never kept
    static const wchar_t* const kWords[] = {L"porn", L"xxx", L"xvideos", L"xhamster", L"xnxx", L"redtube", L"youporn", L"onlyfans", L"chaturbate", L"stripchat",
                                            L"spankbang", L"brazzers", L"hentai", L"rule34", L"nhentai", L"livejasmin", L"bongacams", L"camsoda", L"fansly",
                                            L"eporner", L"tnaflix", L"motherless", L"beeg.", L"erome", L"nsfw"};
    if (host.empty()) return false;
    for (const wchar_t* w : kWords)
        if (host.find(w) != std::wstring::npos) return true;
    return false;
}

std::wstring CleanLink(const std::wstring& url, bool& changed) {
    changed = false;
    size_t q = url.find(L'?');
    if (q == std::wstring::npos) return url;
    size_t hash = url.find(L'#', q);
    std::wstring query = url.substr(q + 1, hash == std::wstring::npos ? std::wstring::npos : hash - q - 1);
    static const wchar_t* const kExact[] = {L"fbclid", L"gclid", L"gclsrc", L"dclid", L"gbraid", L"wbraid", L"msclkid", L"mc_cid", L"mc_eid", L"igshid", L"igsh", L"yclid",
                                            L"_hsenc", L"_hsmi", L"mkt_tok", L"vero_id", L"vero_conv", L"twclid", L"ttclid", L"li_fat_id", L"epik", L"srsltid", L"oly_anon_id",
                                            L"oly_enc_id", L"rb_clickid", L"s_cid", L"ref_src", L"ref_url", L"_ga", L"_gl", L"si", L"wickedid", L"__s", L"ck_subscriber_id"};
    std::wstring kept;
    for (size_t i = 0; i <= query.size();) {
        size_t e = query.find(L'&', i);
        if (e == std::wstring::npos) e = query.size();
        std::wstring part = query.substr(i, e - i), key = Lower(part.substr(0, part.find(L'=')));
        bool drop = Starts(key, L"utm_") || Starts(key, L"pk_") || Starts(key, L"hsa_");
        for (const wchar_t* k : kExact) drop = drop || key == k;
        if (key == L"si" && !Has(Host(url), L"youtu") && !Has(Host(url), L"spotify")) drop = false; // "si" is tracking only there
        if (drop) changed = true;
        else if (!part.empty()) kept += (kept.empty() ? L"" : L"&") + part;
        i = e + 1;
    }
    if (!changed) return url;
    return url.substr(0, q) + (kept.empty() ? L"" : L"?" + kept) + (hash == std::wstring::npos ? L"" : url.substr(hash));
}

// ---- code ----

std::wstring CodeLang(const std::wstring& raw) {
    std::wstring s = raw.substr(0, 4096);
    size_t a = s.find_first_not_of(L" \t\r\n"), z = s.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos || z - a < 5) return {};
    s = s.substr(a, z - a + 1);
    std::wstring low = Lower(s);
    if (Starts(low, L"<?php")) return L"PHP";
    if (Starts(s, L"#!/")) return Has(s, L"python") ? L"Python" : Has(s, L"node") ? L"JavaScript" : L"Shell";
    if (Starts(low, L"<?xml")) return L"XML";
    if ((s[0] == L'{' || s[0] == L'[') && (s.back() == L'}' || s.back() == L']') && Has(s, L"\":")) return L"JSON";
    if (s[0] == L'<' && (Has(s, L"</") || Has(s, L"/>"))) return Has(low, L"<svg") ? L"SVG" : L"HTML";
    if (Starts(low, L"```")) return L"Markdown";

    int lines = 1, ends = 0, indented = 0, words = 0, score = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n') {
            ++lines;
            if (i + 1 < s.size() && (s[i + 1] == L' ' || s[i + 1] == L'\t')) ++indented;
        }
        if ((s[i] == L';' || s[i] == L'{' || s[i] == L'}') && (i + 1 == s.size() || s[i + 1] == L'\n' || s[i + 1] == L'\r')) ++ends;
        if (s[i] == L' ' && i > 0 && iswalpha(s[i - 1])) ++words;
    }
    static const wchar_t* const kSigns[] = {L"=>", L"->", L"::", L"&&", L"||", L"==", L"!=", L"+=", L"();", L"){", L") {", L"};", L"</", L"/>", L":=", L"#include", L"$(", L"${"};
    for (const wchar_t* k : kSigns) score += Has(s, k) ? 1 : 0;
    static const wchar_t* const kStarts[] = {L"def ", L"class ", L"import ", L"from ", L"return ", L"const ", L"let ", L"var ", L"function ", L"fn ", L"func ", L"public ",
                                             L"private ", L"static ", L"using ", L"package ", L"if (", L"for (", L"while (", L"struct ", L"enum ", L"interface ", L"impl ",
                                             L"pub ", L"async ", L"#include", L"#define", L"select ", L"insert into", L"update ", L"create table", L"export ", L"elif ",
                                             L"fun ", L"val ", L"local ", L"echo ", L"sudo ", L"git ", L"npm ", L"pip ", L"cd ", L"curl ", L"winget ", L"get-", L"set-", L"$env:"};
    int starts = 0;
    for (size_t i = 0; i < low.size(); i = low.find(L'\n', i), i = i == std::wstring::npos ? low.size() : i + 1) {
        size_t f = low.find_first_not_of(L" \t", i);
        if (f == std::wstring::npos) break;
        for (const wchar_t* k : kStarts)
            if (low.compare(f, wcslen(k), k) == 0) {
                ++starts;
                break;
            }
    }
    score += starts * 2 + (std::min)(ends, 4) + (indented > 0 && lines > 2 ? 2 : 0);
    bool prose = words > 8 && score < words / 4; // a paragraph with a semicolon is still a paragraph
    bool shellLine = lines == 1 && (Starts(low, L"git ") || Starts(low, L"npm ") || Starts(low, L"pip ") || Starts(low, L"sudo ") || Starts(low, L"winget ") ||
                                    Starts(low, L"cd ") || Starts(low, L"curl ") || Starts(low, L"docker ") || Starts(low, L"cargo ") || Starts(low, L"choco "));
    if (!shellLine && (prose || score < (lines == 1 ? 3 : 4))) return {};

    if (Has(s, L"#include") || Has(s, L"std::") || Has(s, L"template<") || Has(s, L"nullptr")) return Has(s, L"std::") || Has(s, L"class ") || Has(s, L"nullptr") ? L"C++" : L"C";
    if (Has(s, L"using System") || Has(s, L"{ get;") || (Has(s, L"namespace ") && Has(s, L"public class"))) return L"C#";
    if (Has(s, L"public static void main") || Has(s, L"System.out.")) return L"Java";
    if (Has(s, L"fn ") && (Has(s, L"let mut") || Has(s, L"->") || Has(s, L"impl ") || Has(s, L"println!") || Has(s, L"pub "))) return L"Rust";
    if (Has(s, L"package main") || (Has(s, L"func ") && Has(s, L":="))) return L"Go";
    if (Has(s, L"import SwiftUI") || Has(s, L"import UIKit") || Has(s, L"guard let") || (Has(s, L"func ") && Has(s, L"let ") && !Has(s, L";"))) return L"Swift";
    if (Has(s, L"fun ") && (Has(s, L"val ") || Has(s, L"println("))) return L"Kotlin";
    if (Has(s, L"Get-") || Has(s, L"$env:") || Has(s, L"Write-Host") || Has(s, L"-ErrorAction")) return L"PowerShell";
    if (Starts(low, L"select ") || Starts(low, L"insert ") || Starts(low, L"update ") || Starts(low, L"delete from") || Starts(low, L"create ") || Starts(low, L"with "))
        if (Has(low, L" from ") || Has(low, L" into ") || Has(low, L" set ") || Has(low, L"table")) return L"SQL";
    if ((Has(s, L"def ") || Has(s, L"import ") || Has(s, L"elif ") || Has(s, L"self.") || Has(s, L"print(")) && !Has(s, L";") && !Has(s, L"{")) return L"Python";
    if (Has(s, L"puts ") || (Has(s, L"def ") && Has(s, L"\nend"))) return L"Ruby";
    if (Has(s, L"local ") && Has(s, L"end")) return L"Lua";
    if (Has(s, L": string") || Has(s, L": number") || Has(s, L"interface ") || Has(s, L": boolean")) return L"TypeScript";
    if (Has(s, L"=>") || Has(s, L"const ") || Has(s, L"let ") || Has(s, L"function") || Has(s, L"console.") || Has(s, L"require(")) return L"JavaScript";
    if (Has(s, L"{") && Has(s, L":") && Has(s, L";") && !Has(s, L"(") ) return L"CSS";
    if (shellLine || Starts(low, L"$ ") || Has(s, L"echo ") || Has(s, L"export ")) return L"Shell";
    return L"Code";
}

std::vector<Span> Highlight(const std::wstring& s, const std::wstring& lang) {
    static const std::unordered_set<std::wstring> kWords = {
        L"if", L"else", L"for", L"while", L"do", L"return", L"break", L"continue", L"switch", L"case", L"default", L"try", L"catch", L"finally", L"throw", L"new",
        L"delete", L"class", L"struct", L"enum", L"union", L"interface", L"namespace", L"using", L"import", L"from", L"export", L"package", L"public", L"private",
        L"protected", L"static", L"const", L"constexpr", L"final", L"virtual", L"override", L"abstract", L"async", L"await", L"yield", L"let", L"var", L"val",
        L"fn", L"func", L"fun", L"function", L"def", L"lambda", L"pass", L"elif", L"in", L"is", L"not", L"and", L"or", L"as", L"with", L"true", L"false", L"null",
        L"nullptr", L"None", L"True", L"False", L"nil", L"undefined", L"this", L"self", L"super", L"void", L"int", L"bool", L"char", L"float", L"double", L"long",
        L"short", L"unsigned", L"auto", L"string", L"impl", L"pub", L"mut", L"match", L"mod", L"use", L"crate", L"trait", L"where", L"type", L"typeof", L"instanceof",
        L"extends", L"implements", L"go", L"defer", L"chan", L"select", L"guard", L"local", L"then", L"end", L"elseif", L"echo", L"fi", L"esac", L"done", L"template",
        L"typename", L"inline", L"extern", L"sizeof", L"goto", L"readonly", L"sealed", L"override", L"get", L"set", L"of", L"keyof"};
    static const std::unordered_set<std::wstring> kSql = {L"select", L"from", L"where", L"insert", L"into", L"values", L"update", L"set", L"delete", L"create", L"table",
                                                           L"drop", L"alter", L"join", L"left", L"right", L"inner", L"outer", L"on", L"group", L"by", L"order", L"having",
                                                           L"limit", L"and", L"or", L"not", L"null", L"as", L"distinct", L"union", L"with", L"case", L"when", L"then",
                                                           L"else", L"end", L"primary", L"key", L"index", L"is", L"in", L"like", L"between", L"exists", L"desc", L"asc"};
    bool hashComments = lang == L"Python" || lang == L"Shell" || lang == L"Ruby" || lang == L"PowerShell" || lang == L"YAML";
    bool dashComments = lang == L"SQL" || lang == L"Lua";
    bool sql = lang == L"SQL", markup = lang == L"HTML" || lang == L"XML" || lang == L"SVG";
    std::vector<Span> out;
    size_t n = s.size(), i = 0;
    auto add = [&](size_t at, size_t len, Role r) { out.push_back({(uint32_t)at, (uint32_t)len, r}); };
    while (i < n) {
        wchar_t c = s[i];
        if ((c == L'/' && i + 1 < n && s[i + 1] == L'/' && !hashComments) || (c == L'#' && hashComments) || (dashComments && c == L'-' && i + 1 < n && s[i + 1] == L'-')) {
            size_t e = s.find(L'\n', i);
            e = e == std::wstring::npos ? n : e;
            add(i, e - i, kComment), i = e;
        } else if (c == L'/' && i + 1 < n && s[i + 1] == L'*') {
            size_t e = s.find(L"*/", i + 2);
            e = e == std::wstring::npos ? n : e + 2;
            add(i, e - i, kComment), i = e;
        } else if (markup && s.compare(i, 4, L"<!--") == 0) {
            size_t e = s.find(L"-->", i + 4);
            e = e == std::wstring::npos ? n : e + 3;
            add(i, e - i, kComment), i = e;
        } else if (c == L'"' || c == L'\'' || c == L'`') {
            size_t e = i + 1;
            while (e < n && s[e] != c && (c == L'`' || s[e] != L'\n')) e += s[e] == L'\\' ? 2 : 1;
            e = (std::min)(n, e + 1);
            add(i, e - i, kString), i = e;
        } else if (iswdigit(c) && (i == 0 || !(iswalnum(s[i - 1]) || s[i - 1] == L'_'))) {
            size_t e = i + 1;
            while (e < n && (iswalnum(s[e]) || s[e] == L'.' || s[e] == L'_')) ++e;
            add(i, e - i, kNumber), i = e;
        } else if (iswalpha(c) || c == L'_' || c == L'$' || c == L'@') {
            size_t e = i + 1;
            while (e < n && (iswalnum(s[e]) || s[e] == L'_' || (s[e] == L'-' && markup))) ++e;
            std::wstring w = s.substr(i, e - i);
            size_t next = s.find_first_not_of(L" \t", e);
            bool tag = markup && i > 0 && (s[i - 1] == L'<' || s[i - 1] == L'/');
            if (tag || (sql ? kSql.count(Lower(w)) > 0 : kWords.count(w) > 0)) add(i, e - i, kWord);
            else if (next < n && s[next] == L'(') add(i, e - i, kCall);
            i = e;
        } else {
            ++i;
        }
    }
    return out;
}

const wchar_t* ThemeName(int theme) {
    static const wchar_t* const kNames[kThemes] = {L"Halo", L"Night", L"Ember", L"Frost", L"Solar", L"Paper", L"Mono"};
    return kNames[(std::clamp)(theme, 0, kThemes - 1)];
}

uint32_t ThemeInk(int theme, Role r) { // plain, keyword, string, comment, number, call
    static const uint32_t kInks[kThemes][kRoles] = {
        {0xD8DCE6, 0x000000, 0x9BD98B, 0x7A8194, 0xF2B279, 0x8FC7FF}, // halo: keywords in the accent
        {0xABB2BF, 0xC678DD, 0x98C379, 0x7F848E, 0xD19A66, 0x61AFEF},
        {0xF8F8F2, 0xF92672, 0xE6DB74, 0x8C8A75, 0xAE81FF, 0xA6E22E},
        {0xD8DEE9, 0x81A1C1, 0xA3BE8C, 0x6E7A94, 0xB48EAD, 0x88C0D0},
        {0x93A1A1, 0x859900, 0x2AA198, 0x657B83, 0xD33682, 0x268BD2},
        {0x24292E, 0xD73A49, 0x032F62, 0x6A737D, 0x005CC5, 0x6F42C1},
        {0xC8C8C8, 0xFFFFFF, 0xA0A0A0, 0x6E6E6E, 0xE0E0E0, 0xF0F0F0},
    };
    return kInks[(std::clamp)(theme, 0, kThemes - 1)][r < kRoles ? r : kPlain];
}

uint32_t ThemeBack(int theme) {
    static const uint32_t kBacks[kThemes] = {0, 0x282C34, 0x272822, 0x2E3440, 0x002B36, 0xF6F8FA, 0x161616};
    return kBacks[(std::clamp)(theme, 0, kThemes - 1)];
}

// ---- search and words ----

int Fuzzy(const std::wstring& hay, const std::wstring& needle) { // needle lowercased by the caller
    if (needle.empty()) return 0;
    std::wstring h = Lower(hay);
    if (size_t at = h.find(needle); at != std::wstring::npos) return 10000 - (int)(std::min)(at, (size_t)5000); // whole words in it first, earlier is better
    int score = 0, run = 0;
    size_t j = 0;
    for (size_t i = 0; i < h.size() && j < needle.size(); ++i) {
        if (h[i] != needle[j]) {
            run = 0;
            continue;
        }
        bool wordStart = i == 0 || !iswalnum(h[i - 1]);
        score += 1 + run * 4 + (wordStart ? 6 : 0);
        ++run, ++j;
    }
    return j == needle.size() ? score : -1;
}

std::wstring Ago(int64_t at) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    int64_t now = (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL, d = now - at;
    if (d < 60) return L"now";
    if (d < 3600) return std::to_wstring(d / 60) + L"m";
    if (d < 86400) return std::to_wstring(d / 3600) + L"h";
    if (d < 86400 * 7) return std::to_wstring(d / 86400) + L"d";
    return std::to_wstring(d / (86400 * 7)) + L"w";
}

std::wstring Line(const Item& it) {
    if (it.image) return L"Image  ·  " + std::to_wstring(it.w) + L" × " + std::to_wstring(it.h);
    if (it.kind == kFiles || it.kind == kMedia) {
        std::wstring first = it.head.substr(0, it.head.find(L'\n'));
        const wchar_t* slash = wcsrchr(first.c_str(), L'\\');
        std::wstring name = slash && slash[1] ? std::wstring(slash + 1) : first;
        return it.count > 1 ? name + L"  +" + std::to_wstring(it.count - 1) + L" more" : name;
    }
    std::wstring out; // the first line with words, its runs of spaces made one
    size_t a = it.head.find_first_not_of(L" \t\r\n");
    bool space = false;
    for (size_t i = a == std::wstring::npos ? it.head.size() : a; i < it.head.size() && it.head[i] != L'\n' && it.head[i] != L'\r' && out.size() < 300; ++i) {
        bool ws = it.head[i] == L' ' || it.head[i] == L'\t';
        if (!ws || !space) out += ws ? L' ' : it.head[i];
        space = ws;
    }
    return out;
}

const wchar_t* TabName(int tab) {
    static const wchar_t* const kNames[kTabCount] = {L"All", L"Pinned", L"Text", L"Code", L"Links", L"Files", L"Media", L"Colors"};
    return kNames[(std::clamp)(tab, 0, (int)kTabCount - 1)];
}

bool InTab(const Item& it, int tab) {
    static const Kind kOf[kTabCount] = {kText, kText, kText, kCode, kLink, kFiles, kMedia, kColor};
    return tab == kTabAll || (tab == kTabPinned ? it.pinned : it.kind == kOf[(std::clamp)(tab, 0, (int)kTabCount - 1)]);
}

const wchar_t* KeyName(int key) {
    static const wchar_t* const kNames[kKeys] = {L"Off", L"Alt+Shift+V", L"Ctrl+Alt+V", L"Win+Alt+V", L"Ctrl+Shift+V"};
    return kNames[(std::clamp)(key, 0, kKeys - 1)];
}

bool KeyMods(int key, UINT& mods, UINT& vk) {
    static const UINT kMods[kKeys] = {0, MOD_ALT | MOD_SHIFT, MOD_CONTROL | MOD_ALT, MOD_WIN | MOD_ALT, MOD_CONTROL | MOD_SHIFT};
    if (key <= 0 || key >= kKeys) return false;
    mods = kMods[key] | MOD_NOREPEAT, vk = 'V';
    return true;
}

bool MediaFile(const std::wstring& path) {
    static const wchar_t* const kExt[] = {L".png", L".jpg", L".jpeg", L".gif", L".webp", L".bmp", L".heic", L".avif", L".tif", L".tiff", L".svg", L".ico",
                                          L".mp4", L".mov", L".mkv", L".avi", L".webm", L".wmv", L".m4v", L".mp3", L".wav", L".flac", L".m4a", L".aac",
                                          L".ogg", L".opus", L".wma", L".aiff"};
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot || wcschr(dot, L'\\')) return false;
    for (const wchar_t* e : kExt)
        if (!_wcsicmp(dot, e)) return true;
    return false;
}

bool ImageFile(const std::wstring& path) {
    static const wchar_t* const kExt[] = {L".png", L".jpg", L".jpeg", L".gif", L".webp", L".bmp", L".tif", L".tiff", L".ico", L".heic", L".avif"};
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot) return false;
    for (const wchar_t* e : kExt)
        if (!_wcsicmp(dot, e)) return true;
    return false;
}

}
