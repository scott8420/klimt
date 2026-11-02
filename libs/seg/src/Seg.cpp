// libs/seg -- local AI models (s086). See seg/Seg.hpp and docs/ai.md.
#include "seg/Seg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef KLIMT_HAVE_ORT
#include <onnxruntime_cxx_api.h>
#endif

namespace klimt::seg {

namespace fs = std::filesystem;

const std::vector<Model>& catalog() {
    static const std::vector<Model> c = {
        {"subject", "u2netp.onnx", 4574861, "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8",
         "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx",
         "Select Subject -- U²-Net (small): the thing that stands out", 320},
    };
    return c;
}

const Model* model(std::string_view key) {
    for (const auto& m : catalog())
        if (key == m.key) return &m;
    return nullptr;
}

std::string default_dir() {
    if (const char* e = std::getenv("KLIMT_MODELS"); e && *e) return e;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return (fs::path(x) / "klimt" / "models").string();
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".local" / "share" / "klimt" / "models").string();
}

std::string path_of(const Model& m, const std::string& dir) { return (fs::path(dir) / m.file).string(); }

namespace {
struct Seen { std::uint64_t bytes; std::int64_t when; bool good; };
std::mutex g_seen_mutex;
std::map<std::string, Seen> g_seen;   // a file's hash, checked once a run
std::string mb(std::uint64_t b) {
    char s[32];
    std::snprintf(s, sizeof s, "%.1f MB", static_cast<double>(b) / 1e6);
    return s;
}
}  // namespace

State check(const Model& m, const std::string& dir) {
    const std::string p = path_of(m, dir);
    std::error_code ec;
    if (!fs::exists(p, ec)) return State::Missing;
    const auto bytes = fs::file_size(p, ec);
    if (ec || bytes != m.bytes) return State::WrongSize;
    const auto when = static_cast<std::int64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    {
        std::lock_guard<std::mutex> lk(g_seen_mutex);
        if (const auto it = g_seen.find(p); it != g_seen.end() && it->second.bytes == bytes && it->second.when == when)
            return it->second.good ? State::Ready : State::WrongHash;
    }
    const bool good = sha256_file(p) == m.sha256;
    std::lock_guard<std::mutex> lk(g_seen_mutex);
    g_seen[p] = {bytes, when, good};
    return good ? State::Ready : State::WrongHash;
}

std::string state_words(const Model& m, State s, const std::string& dir) {
    const std::string f = std::string(m.file) + " (" + mb(m.bytes) + ")";
    switch (s) {
        case State::Ready: return f + " is ready";
        case State::Missing: return f + " isn't in " + dir;
        case State::WrongSize: return std::string(m.file) + " in " + dir + " is the wrong size -- half downloaded? (it should be " + mb(m.bytes) + ")";
        case State::WrongHash: return std::string(m.file) + " in " + dir + " isn't the file Klimt knows (its SHA-256 differs)";
    }
    return {};
}

namespace {
// Bilinear sample of an interleaved 8-bit RGB picture at (x, y) in its pixels.
[[maybe_unused]] void sample(const std::uint8_t* rgb, int w, int h, double x, double y, float out[3]) {
    x = std::clamp(x, 0.0, w - 1.0);
    y = std::clamp(y, 0.0, h - 1.0);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double fx = x - x0, fy = y - y0;
    for (int c = 0; c < 3; ++c) {
        const auto at = [&](int xx, int yy) { return static_cast<double>(rgb[(static_cast<std::size_t>(yy) * w + xx) * 3 + c]); };
        out[c] = static_cast<float>((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy);
    }
}
}  // namespace

#ifdef KLIMT_HAVE_ORT

namespace {
Ort::Env& env() {
    static Ort::Env e(ORT_LOGGING_LEVEL_ERROR, "klimt");
    return e;
}
std::mutex g_sessions_mutex;
std::map<std::string, std::shared_ptr<Ort::Session>> g_sessions;
std::shared_ptr<Ort::Session> session(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_sessions_mutex);
    if (auto it = g_sessions.find(path); it != g_sessions.end()) return it->second;
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    auto s = std::make_shared<Ort::Session>(env(), path.c_str(), so);
    g_sessions[path] = s;
    return s;
}
}  // namespace

bool available() { return true; }
std::string version() { return Ort::GetVersionString(); }

std::optional<std::vector<float>> salient(const std::string& model_path, int side, const std::uint8_t* rgb, int w, int h,
                                          std::string& error, const std::function<bool()>& cancelled) {
    if (w < 1 || h < 1 || !rgb) { error = "an empty picture"; return std::nullopt; }
    // In: the picture squeezed to side x side, scaled by its brightest value,
    // ImageNet's mean and spread taken out (as U²-Net was trained).
    const int S = side;
    std::vector<float> in(static_cast<std::size_t>(3) * S * S);
    std::vector<float> tmp(static_cast<std::size_t>(3) * S * S);
    float top = 1;
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            float px[3];
            sample(rgb, w, h, (x + 0.5) * w / S - 0.5, (y + 0.5) * h / S - 0.5, px);
            for (int c = 0; c < 3; ++c) {
                tmp[(static_cast<std::size_t>(y) * S + x) * 3 + c] = px[c];
                top = std::max(top, px[c]);
            }
        }
    const float mean[3] = {0.485f, 0.456f, 0.406f}, spread[3] = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x)
            for (int c = 0; c < 3; ++c)
                in[static_cast<std::size_t>(c) * S * S + static_cast<std::size_t>(y) * S + x] =
                    (tmp[(static_cast<std::size_t>(y) * S + x) * 3 + c] / top - mean[c]) / spread[c];
    if (cancelled && cancelled()) { error = "cancelled"; return std::nullopt; }
    std::vector<float> out_small;
    try {
        auto s = session(model_path);
        Ort::AllocatorWithDefaultOptions a;
        auto in_name = s->GetInputNameAllocated(0, a);
        auto out_name = s->GetOutputNameAllocated(0, a);
        const std::int64_t shape[4] = {1, 3, S, S};
        auto mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto t = Ort::Value::CreateTensor<float>(mi, in.data(), in.size(), shape, 4);
        const char* ins[] = {in_name.get()};
        const char* outs[] = {out_name.get()};
        auto r = s->Run(Ort::RunOptions{nullptr}, ins, &t, 1, outs, 1);
        const auto info = r[0].GetTensorTypeAndShapeInfo();
        if (info.GetElementCount() != static_cast<std::size_t>(S) * S) { error = "the model gave back an unexpected shape"; return std::nullopt; }
        const float* o = r[0].GetTensorData<float>();
        out_small.assign(o, o + static_cast<std::size_t>(S) * S);
    } catch (const Ort::Exception& e) {
        error = std::string("ONNX Runtime: ") + e.what();
        return std::nullopt;
    }
    if (cancelled && cancelled()) { error = "cancelled"; return std::nullopt; }
    // Its range stretched to 0..1, then back to the picture's size.
    const auto [lo, hi] = std::minmax_element(out_small.begin(), out_small.end());
    const float l = *lo, span = std::max(1e-6f, *hi - *lo);
    for (auto& v : out_small) v = (v - l) / span;
    std::vector<float> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const double sy = std::clamp((y + 0.5) * S / h - 0.5, 0.0, S - 1.0);
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, S - 1);
        const double fy = sy - y0;
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp((x + 0.5) * S / w - 0.5, 0.0, S - 1.0);
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, S - 1);
            const double fx = sx - x0;
            const auto at = [&](int xx, int yy) { return static_cast<double>(out_small[static_cast<std::size_t>(yy) * S + xx]); };
            out[static_cast<std::size_t>(y) * w + x] =
                static_cast<float>((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy);
        }
    }
    return out;
}

#else

bool available() { return false; }
std::string version() { return {}; }
std::optional<std::vector<float>> salient(const std::string&, int, const std::uint8_t*, int, int, std::string& error,
                                          const std::function<bool()>&) {
    error = "Klimt was built without ONNX Runtime -- install onnxruntime-devel and build again";
    return std::nullopt;
}

#endif

}  // namespace klimt::seg
