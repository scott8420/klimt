#include "tint/Tint.hpp"

#include <lcms2.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>

namespace klimt::tint {

namespace {

void quiet(cmsContext, cmsUInt32Number, const char*) {}   // a bad profile is a nullptr, not a message
struct Quiet { Quiet() { cmsSetLogErrorHandler(quiet); } };
const Quiet kQuiet;

struct Profile {
    cmsHPROFILE h = nullptr;
    explicit Profile(cmsHPROFILE p) : h(p) {}
    ~Profile() { if (h) cmsCloseProfile(h); }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};

Profile open(const Icc& icc) {
    if (icc.empty()) return Profile(nullptr);
    return Profile(cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size())));
}

bool space_of(cmsHPROFILE h, Space& s) {
    switch (cmsGetColorSpace(h)) {
        case cmsSigGrayData: s = Space::Gray; return true;
        case cmsSigRgbData:  s = Space::RGB;  return true;
        case cmsSigCmykData: s = Space::CMYK; return true;
        case cmsSigLabData:  s = Space::Lab;  return true;
        default: return false;
    }
}

cmsUInt32Number type_of(Space s) {
    switch (s) {
        case Space::Gray: return TYPE_GRAY_16;
        case Space::RGB:  return TYPE_RGB_16;
        case Space::CMYK: return TYPE_CMYK_16;
        case Space::Lab:  return TYPE_Lab_16;
    }
    return TYPE_RGB_16;
}

class LcmsLink final : public Link {
public:
    explicit LcmsLink(cmsHTRANSFORM t) : m_t(t) {}
    ~LcmsLink() override { cmsDeleteTransform(m_t); }
    void run(const float* src, float* dst, std::size_t n) const override {
        constexpr std::size_t kChunk = 4096;
        thread_local std::vector<std::uint16_t> a, b;
        for (std::size_t at = 0; at < n; at += kChunk) {
            const std::size_t m = std::min(kChunk, n - at);
            a.resize(m * static_cast<std::size_t>(in));
            b.resize(m * static_cast<std::size_t>(out));
            const float* s = src + at * static_cast<std::size_t>(in);
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] = static_cast<std::uint16_t>(std::lround(std::clamp(s[i], 0.f, 1.f) * 65535.f));
            cmsDoTransform(m_t, a.data(), b.data(), static_cast<cmsUInt32Number>(m));
            float* d = dst + at * static_cast<std::size_t>(out);
            for (std::size_t i = 0; i < b.size(); ++i) d[i] = static_cast<float>(b[i]) / 65535.f;
        }
    }
private:
    cmsHTRANSFORM m_t;
};

std::mutex g_mu;
std::map<std::pair<Icc, int>, std::shared_ptr<const Link>> g_links;   // (profile, space x 2 + direction)

std::shared_ptr<const Link> make(const Icc& icc, Space s, bool to) {
    Profile p = open(icc);
    if (!p.h) return nullptr;
    Space ps;
    if (!space_of(p.h, ps) || ps != s) return nullptr;
    Profile web(cmsCreate_sRGBProfile());
    const cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE;   // NOCACHE: shared across threads
    cmsHTRANSFORM t = to ? cmsCreateTransform(p.h, type_of(s), web.h, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, flags)
                         : cmsCreateTransform(web.h, TYPE_RGB_16, p.h, type_of(s), INTENT_RELATIVE_COLORIMETRIC, flags);
    if (!t) return nullptr;
    auto l = std::make_shared<LcmsLink>(t);
    l->in = to ? channels(s) : 3;
    l->out = to ? 3 : channels(s);
    return l;
}

std::shared_ptr<const Link> cached(const Icc& icc, Space s, bool to) {
    if (icc.empty()) return nullptr;
    const auto key = std::make_pair(icc, static_cast<int>(s) * 2 + (to ? 1 : 0));
    {
        std::lock_guard lk(g_mu);
        if (auto it = g_links.find(key); it != g_links.end()) return it->second;
    }
    auto l = make(icc, s, to);   // made outside the lock: a few ms
    std::lock_guard lk(g_mu);
    if (g_links.size() > 32) g_links.clear();
    return g_links.emplace(key, l).first->second;
}

std::map<std::pair<const Icc*, int>, std::pair<IccRef, std::shared_ptr<const Link>>> g_refs;

std::shared_ptr<const Link> cached_ref(const IccRef& icc, Space s, bool to) {
    if (!icc || icc->empty()) return nullptr;
    const auto key = std::make_pair(icc.get(), static_cast<int>(s) * 2 + (to ? 1 : 0));
    {
        std::lock_guard lk(g_mu);
        if (auto it = g_refs.find(key); it != g_refs.end()) return it->second.second;
    }
    auto l = cached(*icc, s, to);
    std::lock_guard lk(g_mu);
    if (g_refs.size() > 32) g_refs.clear();
    g_refs[key] = {icc, l};
    return l;
}

class LcmsDisplay final : public Display {
public:
    explicit LcmsDisplay(cmsHTRANSFORM t) : m_t(t) {}
    ~LcmsDisplay() override { cmsDeleteTransform(m_t); }
    void run(std::uint32_t* p, std::size_t n) const override {
        // Straight colour for lcms; alpha copied through; premultiplied back.
        thread_local std::vector<std::uint32_t> s;
        s.assign(p, p + n);
        bool partial = false;
        for (auto& v : s) {
            const std::uint32_t a = v >> 24;
            if (a == 255) continue;
            partial = true;
            if (a == 0) { v = 0; continue; }
            const auto un = [a](std::uint32_t c) { return std::min<std::uint32_t>(255, (c * 255 + a / 2) / a); };
            v = (a << 24) | (un(v >> 16 & 0xff) << 16) | (un(v >> 8 & 0xff) << 8) | un(v & 0xff);
        }
        cmsDoTransform(m_t, s.data(), p, static_cast<cmsUInt32Number>(n));
        if (!partial) return;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = s[i] >> 24, v = p[i];
            if (a == 255) continue;
            const auto pm = [a](std::uint32_t c) { return (c * a + 127) / 255; };
            p[i] = (a << 24) | (pm(v >> 16 & 0xff) << 16) | (pm(v >> 8 & 0xff) << 8) | pm(v & 0xff);
        }
    }
private:
    cmsHTRANSFORM m_t;
};

std::map<const Icc*, std::pair<IccRef, std::shared_ptr<const Display>>> g_displays;
std::map<const Icc*, std::pair<IccRef, bool>> g_srgb;

Icc save(cmsHPROFILE h) {
    Icc out;
    if (!h) return out;
    cmsUInt32Number n = 0;
    if (cmsSaveProfileToMem(h, nullptr, &n) && n > 0) {
        out.resize(n);
        if (!cmsSaveProfileToMem(h, out.data(), &n)) out.clear();
    }
    cmsCloseProfile(h);
    return out;
}

void describe(cmsHPROFILE h, const std::string& name) {
    cmsMLU* m = cmsMLUalloc(nullptr, 1);
    cmsMLUsetASCII(m, "en", "US", name.c_str());
    cmsWriteTag(h, cmsSigProfileDescriptionTag, m);
    cmsMLUfree(m);
}

}  // namespace

int channels(Space s) { return s == Space::Gray ? 1 : s == Space::CMYK ? 4 : 3; }

Info inspect(const Icc& icc) {
    Info i;
    Profile p = open(icc);
    if (!p.h || !space_of(p.h, i.space)) return i;
    i.ok = true;
    char buf[256] = {};
    if (cmsGetProfileInfoASCII(p.h, cmsInfoDescription, "en", "US", buf, sizeof buf) > 0) i.name = buf;
    while (!i.name.empty() && (i.name.back() == ' ' || i.name.back() == '\0')) i.name.pop_back();
    return i;
}

std::shared_ptr<const Link> to_srgb(const Icc& icc, Space s) { return cached(icc, s, true); }
std::shared_ptr<const Link> from_srgb(const Icc& icc, Space s) { return cached(icc, s, false); }
std::shared_ptr<const Link> to_srgb(const IccRef& icc, Space s) { return cached_ref(icc, s, true); }
std::shared_ptr<const Link> from_srgb(const IccRef& icc, Space s) { return cached_ref(icc, s, false); }

bool is_srgb(const Icc& icc) {
    if (icc.empty()) return true;   // no profile: taken as sRGB
    const auto l = to_srgb(icc, Space::RGB);
    if (!l) return false;
    // Through the link, a spread of colours must come back as themselves
    // (within half an 8-bit step).
    std::vector<float> in, out;
    for (int r = 0; r < 5; ++r)
        for (int g = 0; g < 5; ++g)
            for (int b = 0; b < 5; ++b) { in.push_back(r / 4.f); in.push_back(g / 4.f); in.push_back(b / 4.f); }
    out.resize(in.size());
    l->run(in.data(), out.data(), in.size() / 3);
    for (std::size_t i = 0; i < in.size(); ++i)
        if (std::abs(in[i] - out[i]) > 0.5f / 255) return false;
    return true;
}

bool is_srgb(const IccRef& icc) {
    if (!icc) return true;
    {
        std::lock_guard lk(g_mu);
        if (auto it = g_srgb.find(icc.get()); it != g_srgb.end()) return it->second.second;
    }
    const bool v = is_srgb(*icc);
    std::lock_guard lk(g_mu);
    if (g_srgb.size() > 32) g_srgb.clear();
    g_srgb[icc.get()] = {icc, v};
    return v;
}

std::shared_ptr<const Display> display(const IccRef& icc) {
    if (!icc || icc->empty()) return nullptr;
    {
        std::lock_guard lk(g_mu);
        if (auto it = g_displays.find(icc.get()); it != g_displays.end()) return it->second.second;
    }
    std::shared_ptr<const Display> d;
    if (!is_srgb(icc)) {
        Profile p = open(*icc);
        Space ps;
        if (p.h && space_of(p.h, ps) && ps == Space::RGB) {
            Profile web(cmsCreate_sRGBProfile());
            const bool little = std::endian::native == std::endian::little;
            const cmsUInt32Number fmt = little ? TYPE_BGRA_8 : TYPE_ARGB_8;
            if (cmsHTRANSFORM t = cmsCreateTransform(p.h, fmt, web.h, fmt, INTENT_RELATIVE_COLORIMETRIC,
                                                     cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA))
                d = std::make_shared<LcmsDisplay>(t);
        }
    }
    std::lock_guard lk(g_mu);
    if (g_displays.size() > 32) g_displays.clear();
    g_displays[icc.get()] = {icc, d};
    return d;
}

std::shared_ptr<const Link> link(const Icc& from, Space fs, const Icc& to, Space ts) {
    Profile a = open(from), b = open(to);
    Space sa, sb;
    if (!a.h || !b.h || !space_of(a.h, sa) || !space_of(b.h, sb) || sa != fs || sb != ts) return nullptr;
    cmsHTRANSFORM t = cmsCreateTransform(a.h, type_of(fs), b.h, type_of(ts), INTENT_RELATIVE_COLORIMETRIC,
                                         cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    if (!t) return nullptr;
    auto l = std::make_shared<LcmsLink>(t);
    l->in = channels(fs);
    l->out = channels(ts);
    return l;
}

Icc implicit(Space s) {
    switch (s) {
        case Space::RGB: return srgb();
        case Space::Gray: {
            const cmsFloat64Number p[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};   // sRGB's curve
            cmsToneCurve* c = cmsBuildParametricToneCurve(nullptr, 4, p);
            cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), c);
            cmsFreeToneCurve(c);
            if (h) describe(h, "Gray (sRGB curve)");
            return save(h);
        }
        case Space::CMYK: return plain_cmyk();
        case Space::Lab: {
            cmsHPROFILE h = cmsCreateLab4Profile(nullptr);
            if (h) describe(h, "Lab D50");
            return save(h);
        }
    }
    return srgb();
}

Icc srgb() {
    cmsHPROFILE h = cmsCreate_sRGBProfile();
    if (h) describe(h, "sRGB IEC61966-2.1");
    return save(h);
}

Icc gray(double gamma) {
    cmsToneCurve* c = cmsBuildGamma(nullptr, gamma);
    cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), c);
    cmsFreeToneCurve(c);
    if (h) describe(h, "Gray Gamma " + std::to_string(gamma).substr(0, 3));
    return save(h);
}

Icc rgb(const double p[6], double gamma, const std::string& name) {
    cmsCIExyY white;
    cmsWhitePointFromTemp(&white, 6504);
    const cmsCIExyYTRIPLE prim{{p[0], p[1], 1}, {p[2], p[3], 1}, {p[4], p[5], 1}};
    cmsToneCurve* c = cmsBuildGamma(nullptr, gamma);
    cmsToneCurve* three[3] = {c, c, c};
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &prim, three);
    cmsFreeToneCurve(c);
    if (h) describe(h, name);
    return save(h);
}

Icc adobe_rgb() {
    const double p[6] = {0.64, 0.33, 0.21, 0.71, 0.15, 0.06};
    return rgb(p, 563.0 / 256.0, "Adobe RGB (1998) compatible");
}

Icc plain_cmyk() {
    // CMYK -> Lab (AToB0) and Lab -> CMYK (BToA0) as 3D / 4D tables: the
    // textbook (1-c)(1-k) through sRGB, and back with full black generation.
    cmsHPROFILE h = cmsCreateProfilePlaceholder(nullptr);
    if (!h) return {};
    cmsSetProfileVersion(h, 4.3);
    cmsSetDeviceClass(h, cmsSigOutputClass);
    cmsSetColorSpace(h, cmsSigCmykData);
    cmsSetPCS(h, cmsSigLabData);
    describe(h, "Plain CMYK (no dot gain)");
    cmsWriteTag(h, cmsSigMediaWhitePointTag, const_cast<cmsCIEXYZ*>(cmsD50_XYZ()));
    Profile web(cmsCreate_sRGBProfile());
    Profile lab(cmsCreateLab4Profile(nullptr));
    cmsHTRANSFORM to_lab = cmsCreateTransform(web.h, TYPE_RGB_DBL, lab.h, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
    cmsHTRANSFORM from_lab = cmsCreateTransform(lab.h, TYPE_Lab_DBL, web.h, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
    struct Ctx { cmsHTRANSFORM to, from; } ctx{to_lab, from_lab};
    // AToB: CMYK (0..1 ink) -> Lab (encoded 0..1 per lcms' v4 float convention)
    cmsPipeline* a2b = cmsPipelineAlloc(nullptr, 4, 3);
    cmsStage* clut_a = cmsStageAllocCLut16bit(nullptr, 13, 4, 3, nullptr);
    cmsStageSampleCLut16bit(clut_a, [](const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) -> cmsInt32Number {
        auto* c = static_cast<Ctx*>(cargo);
        const double k = in[3] / 65535.0;
        double rgb[3];
        for (int i = 0; i < 3; ++i) rgb[i] = (1 - in[i] / 65535.0) * (1 - k);
        cmsCIELab L;
        cmsDoTransform(c->to, rgb, &L, 1);
        cmsFloat2LabEncoded(out, &L);
        return 1;
    }, &ctx, 0);
    // mAB wants curves round the table: A curves, CLUT, B curves (identity).
    cmsPipelineInsertStage(a2b, cmsAT_END, cmsStageAllocToneCurves(nullptr, 4, nullptr));
    cmsPipelineInsertStage(a2b, cmsAT_END, clut_a);
    cmsPipelineInsertStage(a2b, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, nullptr));
    cmsWriteTag(h, cmsSigAToB0Tag, a2b);
    cmsWriteTag(h, cmsSigAToB1Tag, a2b);
    // BToA: Lab -> CMYK, through sRGB, all the black taken into K.
    cmsPipeline* b2a = cmsPipelineAlloc(nullptr, 3, 4);
    cmsStage* clut_b = cmsStageAllocCLut16bit(nullptr, 33, 3, 4, nullptr);
    cmsStageSampleCLut16bit(clut_b, [](const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) -> cmsInt32Number {
        auto* c = static_cast<Ctx*>(cargo);
        cmsCIELab L;
        cmsLabEncoded2Float(&L, in);
        double rgb[3];
        cmsDoTransform(c->from, &L, rgb, 1);
        for (auto& v : rgb) v = std::clamp(v, 0.0, 1.0);
        const double k = 1 - std::max({rgb[0], rgb[1], rgb[2]}), d = 1 - k;
        for (int i = 0; i < 3; ++i) out[i] = static_cast<cmsUInt16Number>(std::lround((d > 0 ? (d - rgb[i]) / d : 0.0) * 65535));
        out[3] = static_cast<cmsUInt16Number>(std::lround(k * 65535));
        return 1;
    }, &ctx, 0);
    cmsPipelineInsertStage(b2a, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, nullptr));
    cmsPipelineInsertStage(b2a, cmsAT_END, clut_b);
    cmsPipelineInsertStage(b2a, cmsAT_END, cmsStageAllocToneCurves(nullptr, 4, nullptr));
    cmsWriteTag(h, cmsSigBToA0Tag, b2a);
    cmsWriteTag(h, cmsSigBToA1Tag, b2a);
    cmsPipelineFree(a2b);
    cmsPipelineFree(b2a);
    cmsDeleteTransform(to_lab);
    cmsDeleteTransform(from_lab);
    return save(h);
}

}  // namespace klimt::tint
