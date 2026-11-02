#pragma once
// klimt::seg -- LOCAL AI MODELS (s086; REQUIREMENTS "Added": Select Subject,
// background matting, LaMa fill; docs/ai.md). Models run on this machine
// through ONNX Runtime -- nothing leaves it. GTK-free; every call is pure
// over its arguments and safe on a worker (a session is shared and cached;
// ONNX Runtime's Run is thread safe).
//
// THE MODEL STORE: the models aren't in the distro, so they live as files in
// one folder (`default_dir()`: $KLIMT_MODELS, else
// $XDG_DATA_HOME/klimt/models, else ~/.local/share/klimt/models). The
// catalog names each one: its file, its exact size and SHA-256 (checked once
// a run, so a half-downloaded or wrong file is said, not run), where it comes
// from, and what it's for.
//
// ONNX Runtime is optional at build time (KLIMT_HAVE_ORT): without it
// `available()` is false and every run refuses in words.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::seg {

bool available();
std::string version();   // "1.20.1"; "" without ONNX Runtime

struct Model {
    const char*   key;      // "subject"
    const char*   file;     // "u2netp.onnx"
    std::uint64_t bytes;
    const char*   sha256;   // lower-case hex
    const char*   url;      // where it is published
    const char*   what;     // "Select Subject -- U²-Net (small), salient object"
    int           side;     // the square it sees (320)
};
const std::vector<Model>& catalog();
const Model* model(std::string_view key);

std::string default_dir();
std::string path_of(const Model& m, const std::string& dir);
enum class State { Ready, Missing, WrongSize, WrongHash };
// Is the model there and whole? The hash is read once a run per file (its
// size and change time remembered).
State check(const Model& m, const std::string& dir);
std::string state_words(const Model& m, State s, const std::string& dir);   // "u2netp.onnx (4.4 MB) isn't in ~/.local/share/klimt/models"

std::string sha256_file(const std::string& path);   // "" when unreadable
std::string sha256(const std::uint8_t* data, std::size_t n);

// SALIENT OBJECT: what stands out in the picture. `rgb`: w x h, 8-bit,
// interleaved RGB. Back: w x h coverage 0..1 (the model's own soft edge),
// or nullopt with `error` in words (cancelled: "cancelled").
std::optional<std::vector<float>> salient(const std::string& model_path, int side, const std::uint8_t* rgb, int w, int h,
                                          std::string& error, const std::function<bool()>& cancelled = {});

}  // namespace klimt::seg
