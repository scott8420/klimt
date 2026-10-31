# Klimt libraries

Klimt's work lives in libraries, each GTK-free, with its own `CMakeLists.txt`
and tests, the way Composix carries `libs/plate`. The app is a thin shell over
them. Long jobs run on worker threads, never on the UI thread
(docs/REQUIREMENTS.md, "Threading").

Built: `pixel` (s006), `formats` (s007), `text` (s046: type on Pango + Cairo). Planned, each added only when a milestone needs it:

| Library | Holds | Shared with |
|---|---|---|
| `pixel`   | tiles, colour depths, blend modes, compositing | -- |
| `tint`    | colour management: ICC, CMYK, spot inks | Composix |
| `trace`   | raster -> bezier outlines (Streamline-style) | Granjon, Curvz |
| `formats` | PNG, JPEG, TIFF (s007); PSD, ORA, EPS, WebP, RAW to come | -- |
| `filters` | adjustments and effects | -- |
| `seg`     | local models (SAM, matting, inpaint) via ONNX Runtime | -- |
| `plate`   | PDF writer -- copied from Composix when export needs it | Composix |

Names are working names.
