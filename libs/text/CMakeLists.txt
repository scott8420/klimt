# libs/text -- Klimt's type (s046): a live text layer's settings, set with
# Pango + Cairo. GTK-free (pangocairo comes with GTK, but nothing here needs
# it), so klimt_core links it and stays headless. Its checks live in
# klimt_selftest.
#   Fedora: pango-devel cairo-devel (both pulled in by gtkmm4.0-devel)
#   Ubuntu: libpango1.0-dev libcairo2-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo>=1.44)
# s048: hyphenation -- Composix's Liang (s038, s190), the TeX patterns of
# ../../third_party/hyphen embedded at build time (no package at run time).
set(KT_HYPH_LANGS "en-US:en-us" "en-GB:en-gb" "de:de-1996" "fr:fr" "es:es" "it:it" "nl:nl" "pt:pt")
set(KT_HYPH_SETS "")
set(KT_HYPH_COUNT 0)
set(KT_HYPH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/hyphen)
foreach(pair IN LISTS KT_HYPH_LANGS)
    string(REPLACE ":" ";" parts "${pair}")
    list(GET parts 0 code)
    list(GET parts 1 file)
    set(pat ${KT_HYPH_DIR}/hyph-${file}.pat.txt)
    set(hyp ${KT_HYPH_DIR}/hyph-${file}.hyp.txt)
    file(READ ${pat} P)
    set(E "")
    if(EXISTS ${hyp})
        file(READ ${hyp} E)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${hyp})
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pat})
    string(APPEND KT_HYPH_SETS "    {\"${code}\", R\"HYPH(${P})HYPH\", R\"HYPH(${E})HYPH\"},\n")
    math(EXPR KT_HYPH_COUNT "${KT_HYPH_COUNT} + 1")
endforeach()
configure_file(src/HyphenData.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/HyphenData.cpp @ONLY)
add_library(klimt_text STATIC src/Text.cpp src/Hyphen.cpp ${CMAKE_CURRENT_BINARY_DIR}/HyphenData.cpp)
target_include_directories(klimt_text PUBLIC include)
target_link_libraries(klimt_text PRIVATE PkgConfig::PANGOCAIRO)
target_compile_features(klimt_text PUBLIC cxx_std_23)
# Pixel-free and fast enough at -O2 even in Debug (setting runs per keystroke).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(klimt_text PRIVATE -O2)
endif()
