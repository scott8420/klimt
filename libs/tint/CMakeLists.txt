# libs/tint -- colour management (s067; WEAKNESSES 14): ICC profiles through
# Little CMS 2. GTK-free. Its checks live in klimt_selftest.
#   Fedora: lcms2-devel   Ubuntu: liblcms2-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2>=2.9)
add_library(klimt_tint STATIC src/Tint.cpp)
target_include_directories(klimt_tint PUBLIC include)
target_link_libraries(klimt_tint PRIVATE PkgConfig::LCMS2)
target_compile_features(klimt_tint PUBLIC cxx_std_23)
target_compile_options(klimt_tint PRIVATE -O2)
