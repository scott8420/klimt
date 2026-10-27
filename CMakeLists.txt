cmake_minimum_required(VERSION 3.20)
project(klimt VERSION 0.1.0 LANGUAGES C CXX)

# Klimt -- a raster image editor for GNOME (gtkmm 4). Started s001 from the
# Cairn spine v1. The build string the About window and install.sh show:
set(KLIMT_BUILD "s006")

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
)
target_include_directories(klimt_core PUBLIC include)
target_link_libraries(klimt_core PUBLIC klimt_pixel)
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(klimt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(klimt_selftest src/selftest.cpp)
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
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/scalable/apps/*.svg")
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
    src/AboutWindow.cpp
    src/ShortcutsDialog.cpp
    # s003 -- the Layers panel on core::Document (Pole B, rebuilt per change).
    src/LayersPanel.cpp
    # s005 -- the window's regions, one class each (docs/ui.md).
    src/ToolRail.cpp
    src/ContextStrip.cpp
    src/CanvasView.cpp
    src/PropertiesPanel.cpp
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
