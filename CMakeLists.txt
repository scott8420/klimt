cmake_minimum_required(VERSION 3.20)
project(klimt VERSION 0.1.0 LANGUAGES C CXX)

# Klimt -- a raster image editor for GNOME (gtkmm 4). Started s001 from the
# Cairn spine v1. The build string the About window and install.sh show:
set(KLIMT_BUILD "s035")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Faster builds, each tool only when it's installed (none is required) --
# carried from Composix s194.
#  ccache  -- a rebuild of an unchanged file is a cache hit, even after
#             rm -rf build. KLIMT_CCACHE=OFF to leave it out.
#  mold / lld -- faster links than ld.bfd. KLIMT_LINKER=bfd keeps the default.
#  Ninja is build.sh's choice (it passes -G Ninja when ninja is there).
option(KLIMT_CCACHE "Compile through ccache when it is installed" ON)
if(KLIMT_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        message(STATUS "ccache: ${CCACHE_PROGRAM}")
    endif()
endif()
set(KLIMT_LINKER "" CACHE STRING "mold, lld or bfd (empty: the fastest one installed)")
if(NOT KLIMT_LINKER)
    find_program(MOLD_PROGRAM mold)
    find_program(LLD_PROGRAM ld.lld)
    if(MOLD_PROGRAM)
        set(KLIMT_LINKER_USE mold)
    elseif(LLD_PROGRAM)
        set(KLIMT_LINKER_USE lld)
    endif()
else()
    set(KLIMT_LINKER_USE ${KLIMT_LINKER})
endif()
if(KLIMT_LINKER_USE AND NOT KLIMT_LINKER_USE STREQUAL "bfd")
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=${KLIMT_LINKER_USE}" KLIMT_HAS_LD_${KLIMT_LINKER_USE})
    if(KLIMT_HAS_LD_${KLIMT_LINKER_USE})
        add_link_options("-fuse-ld=${KLIMT_LINKER_USE}")
        message(STATUS "linker: ${KLIMT_LINKER_USE}")
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
# s005: libadwaita (Scott: the modern GNOME look -- docs/ui.md call 6). C API
# only (there is no C++ binding); 1.4 is the floor (AdwOverlaySplitView).
pkg_check_modules(ADW REQUIRED libadwaita-1>=1.4)
find_package(spdlog REQUIRED)
# Long jobs run off the UI thread from the start (REQUIREMENTS: threading).
find_package(Threads REQUIRED)

# ── libs/ -- the image libraries, each its own CMakeLists (s006: pixel first)
add_subdirectory(libs/pixel)
add_subdirectory(libs/formats)   # s007: PNG / JPEG / TIFF in and out

# ── klimt_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable. (docs/two-layer-seam.md.)
add_library(klimt_core STATIC
    src/core/Recents.cpp
    src/core/TextMap.cpp
    # The SHORTCUT-REGISTRY paradigm: one list, two consumers (App wires accels
    # from it, ShortcutsDialog renders from it) so a key and its advertised row
    # can't drift. GTK-free, so it lives in core and the selftest guards it
    # headless -- a pure test proves no two chords collide.
    src/core/Shortcuts.cpp
    # The UNDO paradigm (Cairn s046): core/Undo.hpp is the journal (header-only,
    # a template over the app's store); Stones is Cairn's demo model + its verbs
    # + the verb table the selftest gate walks -- it stays until Klimt's own
    # document replaces it. docs/undo.md.
    src/core/Stones.cpp
    # Klimt's DOCUMENT (s002): the layer tree -- layers and groups, order,
    # visibility, locks, opacity / fill, blend modes, clipping -- every write
    # through the journal, every verb in layers::verbs() for the selftest gate.
    src/core/Document.cpp
    # THE UI NEVER WAITS (s002): the worker pool. A job computes off-thread and
    # LANDS on the UI thread (pump) as one undo step; a cancel leaves nothing.
    src/core/Jobs.cpp
    # The TOOL REGISTRY (s005): the rail, the tool keys and the shortcut
    # reference read one list.
    src/core/Tools.cpp
    # The COMMAND CATALOG + matcher the palette searches (s005).
    src/core/Commands.cpp
    # The COMPOSITOR (s006): the layer tree flattened, per tile, on a worker.
    src/core/Composite.cpp
    # FILES (s007): .klimt in and out (a zip, libzip), images in, File > New,
    # Export -- all pure over a snapshot, so they run on a worker.
    src/core/File.cpp
    # THE SELECTION (s008): a mask on the tile grid; shapes, set arithmetic,
    # feather, the ants' outline, fill / delete through it.
    src/core/Selection.cpp
    # PATHS (s035): Bezier outlines for selections and masks -- geometry, verbs.
    src/core/Path.cpp
    src/core/Paths.cpp
    src/core/Isolate.cpp   # s035 m7: the path as a hint -- GrabCut over the band (core/MaxFlow.hpp)
    # THE BRUSH (s008 m4): dabs into a stroke's coverage, the layer
    # recomputed from the original per touched tile -- one stroke, one step.
    src/core/Paint.cpp
    src/core/Move.cpp      # s009 m1: the Move tool's pixel work
    src/core/Canvas.cpp    # s009 m2: Crop, Canvas Size, Image Size
    src/core/Transform.cpp # s011 m1: Free Transform, flips, rotations
    src/core/Mask.cpp      # s012 m2: layer masks
    src/core/Gradient.cpp  # s012 m4: the Gradient tool
    src/core/Adjust.cpp       # s013: adjustments -- the maths
    src/core/Adjustments.cpp  # s013: adjustment layer verbs (s014: apply_filter)
    src/core/Filter.cpp       # s014: the Filter menu -- the maths
    src/core/StylesRender.cpp # s019: layer styles -- drawing them
    src/core/Live.cpp         # s018: live filters -- the stack, its cache, its verbs
    src/core/Heal.cpp         # s015 m3: the healing brushes
    src/core/Fade.cpp         # s020: Edit > Fade
    src/core/BrushPresets.cpp # s021: saved brushes
    src/core/Transfer.cpp     # s022: layers between documents
    src/core/Session.cpp      # s023: the open tabs, remembered
    src/core/ColourRange.cpp  # s024: Select > Color Range
    src/core/ColourChannels.cpp # s026: the colour channels
    src/core/Patterns.cpp     # s028 m4: the built-in patterns (Pattern Overlay)
    src/core/ToolDefaults.cpp # s029: each tool's saved defaults
    src/core/Rulers.cpp       # s031: units, ruler ticks, the zero point
    src/core/Prefs.cpp        # s014 m4: the app's settings (the dock divider)
)
target_include_directories(klimt_core PUBLIC include)
target_link_libraries(klimt_core PUBLIC klimt_pixel klimt_formats)

# s010 m3: the PIXEL HOT PATHS are optimised in every build type -- a Debug
# build composited ~4x slower than Release, and Debug is what build.sh makes.
# The UI and the rest of the core stay -O0 (debuggable). KLIMT_FAST_PIXELS=OFF
# to step through these too.
option(KLIMT_FAST_PIXELS "Optimise the pixel hot paths even in Debug" ON)
if(KLIMT_FAST_PIXELS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(klimt_pixel PRIVATE -O2)
    set_source_files_properties(src/core/Composite.cpp src/core/Paint.cpp src/core/Canvas.cpp
                                src/core/Move.cpp src/core/Selection.cpp src/core/Transform.cpp src/core/Gradient.cpp
                                src/core/Adjust.cpp src/core/Filter.cpp src/core/Heal.cpp src/core/StylesRender.cpp src/core/Fade.cpp src/core/Patterns.cpp
                                src/core/Path.cpp src/core/Paths.cpp src/core/Isolate.cpp
                                PROPERTIES COMPILE_OPTIONS "-O2")
endif()
# s007: libzip writes and reads the .klimt container (Fedora: libzip-devel).
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip>=1.6)
target_link_libraries(klimt_core PRIVATE PkgConfig::LIBZIP)
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(klimt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── s010: the compositor's speed, headless (./build/klimt_bench [8|16|32]) ──────
add_executable(klimt_bench src/bench.cpp)
target_link_libraries(klimt_bench PRIVATE klimt_core Threads::Threads)

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(klimt_selftest src/selftest.cpp src/selftest_formats.cpp src/selftest_select.cpp src/selftest_move.cpp src/selftest_canvas.cpp src/selftest_pen.cpp src/selftest_bounds.cpp src/selftest_transform.cpp src/selftest_masks.cpp src/selftest_gradient.cpp src/selftest_adjust.cpp src/selftest_filter.cpp src/selftest_retouch.cpp src/selftest_brush.cpp src/selftest_styles.cpp src/selftest_tabs.cpp src/selftest_fade.cpp src/selftest_partly.cpp src/selftest_presets.cpp src/selftest_transfer.cpp src/selftest_session.cpp src/selftest_groups.cpp src/selftest_range.cpp src/selftest_channels.cpp src/selftest_dynamics.cpp src/selftest_tooldefaults.cpp src/selftest_filterlayers.cpp src/selftest_rulers.cpp src/selftest_sharpen.cpp src/selftest_paths.cpp)
target_link_libraries(klimt_selftest PRIVATE klimt_core Threads::Threads)
# The selftest checks every tool's icon is in the resource bundle (s005).
target_compile_definitions(klimt_selftest PRIVATE KLIMT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ── App resources (gresource) -- the RESOURCE-PIPELINE paradigm ────────────────
# glib-compile-resources embeds resources/klimt.gresource.xml into a C source;
# App::on_activate registers it and points the icon theme at the bundle so
# set_icon_name("klimt-logo-symbolic") resolves out of the binary. The generated
# .c compiles as C (project enables C above) so klimt_resources_get_resource()
# has C linkage, matching the extern "C" prototype in App.cpp. Adding an icon:
# drop the .svg in resources/icons/scalable/apps/, list it in the .xml, rebuild.
# (docs/resources-and-dialog-lifetime.md)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(KLIMT_GRESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/resources/klimt.gresource.xml")
set(KLIMT_RESOURCES_C   "${CMAKE_CURRENT_BINARY_DIR}/klimt-resources.c")
file(GLOB KLIMT_RESOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/scalable/apps/*.svg"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/images/*.jpg"   # s027: the About painting
     "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")                  # s027: the License tab
add_custom_command(
    OUTPUT  "${KLIMT_RESOURCES_C}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${KLIMT_RESOURCES_C}"
            --generate-source
            --c-name klimt_resources
            klimt.gresource.xml
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    DEPENDS "${KLIMT_GRESOURCE_XML}" ${KLIMT_RESOURCE_FILES}
    COMMENT "Compiling gresource bundle (klimt-resources.c)"
    VERBATIM)

# ── klimt -- the GTK app ───────────────────────────────────────────────────────
# The Shell is a SPLIT CLASS across TUs by category (see the index in
# include/Shell.hpp); all parts compile into the one klimt::Shell.
add_executable(klimt
    ${KLIMT_RESOURCES_C}
    src/main.cpp
    src/App.cpp
    src/Shell.cpp
    src/Shell_zones.cpp
    src/Shell_bindings.cpp
    src/Shell_handlers.cpp
    src/Shell_helpers.cpp
    src/Shell_files.cpp
    src/Shell_select.cpp
    src/Shell_paint.cpp
    src/Shell_move.cpp
    src/Shell_canvas.cpp
    src/Shell_transform.cpp
    src/Shell_masks.cpp
    src/Shell_tabs.cpp
    src/Shell_adjust.cpp      # s013: adjustment layers, Image > Adjustments
    src/Shell_paths.cpp src/Shell_filter.cpp      # s014: the Filter menu
    src/SizeDialog.cpp
    src/NewDialog.cpp
    src/NumberDialog.cpp
    src/AboutWindow.cpp
    src/ShortcutsDialog.cpp
    # s003 -- the Layers panel on core::Document (Pole B, rebuilt per change).
    src/LayersPanel.cpp
    # s005 -- the window's regions, one class each (docs/ui.md).
    src/ToolRail.cpp
    src/ContextStrip.cpp
    src/CanvasView.cpp src/CanvasView_pen.cpp
    src/PropertiesPanel.cpp
    src/AdjustEditor.cpp      # s013: the adjustment controls
    src/FilterLayerEditor.cpp   # s030: a filter layer's settings in Properties
    src/RulerView.cpp   # s031: the rulers and their corner
    src/AdjustDialog.cpp      # s013: Image > Adjustments
    src/FilterDialog.cpp      # s014: the Filter menu
    src/FadeDialog.cpp        # s020: Edit > Fade
    src/Shell_fade.cpp        # s020: Edit > Fade
    src/ColourRangeDialog.cpp # s024: Select > Color Range
    src/Shell_range.cpp       # s024: Select > Color Range
    src/Shell_channels.cpp    # s026: the colour channels
    src/ChannelsPanel.cpp     # s016: saved selections
    src/StylesEditor.cpp      # s019: layer styles in Properties
    src/StatusLine.cpp
    src/HistoryPanel.cpp
    src/CommandPalette.cpp
    src/EdgeTab.cpp
    src/Registry.cpp
    src/Log.cpp
)
target_include_directories(klimt PRIVATE include ${GTKMM_INCLUDE_DIRS} ${ADW_INCLUDE_DIRS})
target_link_libraries(klimt     PRIVATE ${GTKMM_LIBRARIES} ${ADW_LIBRARIES} klimt_core spdlog::spdlog Threads::Threads)
target_compile_definitions(klimt PRIVATE KLIMT_BUILD="${KLIMT_BUILD}")
target_link_directories(klimt   PRIVATE ${GTKMM_LIBRARY_DIRS} ${ADW_LIBRARY_DIRS})
target_compile_options(klimt    PRIVATE ${GTKMM_CFLAGS_OTHER})

# ── libs/ -- the image libraries (pixel, tint, trace, formats, filters, seg)
# land here one at a time, each with its own CMakeLists.txt and tests, the way
# Composix carries libs/plate. None exists yet (s001); see libs/README.md.
