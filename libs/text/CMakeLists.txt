# libs/text -- Klimt's type (s046): a live text layer's settings, set with
# Pango + Cairo. GTK-free (pangocairo comes with GTK, but nothing here needs
# it), so klimt_core links it and stays headless. Its checks live in
# klimt_selftest.
#   Fedora: pango-devel cairo-devel (both pulled in by gtkmm4.0-devel)
#   Ubuntu: libpango1.0-dev libcairo2-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo>=1.44)
add_library(klimt_text STATIC src/Text.cpp)
target_include_directories(klimt_text PUBLIC include)
target_link_libraries(klimt_text PRIVATE PkgConfig::PANGOCAIRO)
target_compile_features(klimt_text PUBLIC cxx_std_23)
# Pixel-free and fast enough at -O2 even in Debug (setting runs per keystroke).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(klimt_text PRIVATE -O2)
endif()
