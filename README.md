# Klimt

A raster image editor for GNOME, in C++23 and gtkmm 4: layers and groups,
the full set of blend modes, masks, live text layers, non-destructive
adjustments and effects, strong automatic masking, clipping paths, and print
colour (CMYK, spot inks, duotones). Pixels only -- vector drawing belongs to
Curvz, page layout to Composix.

App id `io.github.scott8420.Klimt`. Started s001 from the Cairn spine v1.
See `docs/REQUIREMENTS.md` for what it is to become.

## Build, test, run

```
./build.sh
./build/klimt_selftest     # the GTK-free core, headless
./build/klimt
```

Deps (Fedora): `gtkmm4.0-devel libadwaita-devel spdlog-devel libpng-devel libjpeg-turbo-devel libtiff-devel libzip-devel cmake gcc-c++ zip`.
(Ubuntu/Debian: `libgtkmm-4.0-dev libadwaita-1-dev libspdlog-dev libpng-dev libjpeg-turbo8-dev libtiff-dev libzip-dev cmake g++ zip`.)
Optional, for faster builds: `ccache ninja-build mold`.

## Install / uninstall

```
./install.sh                 # ~/.local, no sudo
./install.sh --system        # /usr/local, asks for sudo
./install.sh --prefix DIR    # anywhere else
./install.sh --build         # build first, then install
./uninstall.sh [--system | --prefix DIR] [--purge]
```

install.sh writes a manifest; uninstall.sh removes exactly what it lists.
`--purge` also removes settings, recents and cache -- never your images.

## Layout

```
klimt/
  include/  src/          the app (Shell split by category) and core/ (GTK-free)
  libs/                   the image libraries, each with its own CMake + tests
  resources/              gresource bundle -- icons ride inside the binary
  data/                   .desktop.in and the app icon (install.sh uses them)
  third_party/            vendored headers (nlohmann/json)
```

## Inherited from Cairn

The paradigms Cairn carries, still here and still exercised by the selftest:
`Named<W>` + live registry, per-area spdlog, the split Shell, actions and the
shortcut registry, the JSON recents pump, the model/UI seam (core/ has no
GTK on its link line), the gresource icon pipeline, the hide-on-close About
window, the undo journal (`core/Undo.hpp`, with Cairn's Stones demo model
until Klimt's document replaces it), and following the desktop's light/dark preference. The docs in
`docs/` explain each.

## License

MIT -- see `LICENSE`.
