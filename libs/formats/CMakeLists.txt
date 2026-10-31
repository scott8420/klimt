# libs/formats -- image files in and out (s007): PNG, JPEG and TIFF on their
# own libraries (16-bit, CMYK and ICC need them; GdkPixbuf is 8-bit RGB).
# GTK-free. Its checks live in klimt_selftest.
#   Fedora: libpng-devel libjpeg-turbo-devel libtiff-devel libwebp-devel
#   Ubuntu: libpng-dev libjpeg-turbo8-dev (or libjpeg-dev) libtiff-dev
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
# s049: WebP -- libwebp, its demux (the ICC profile, animations) and mux (writing the profile).
#   Fedora: libwebp-devel   Ubuntu: libwebp-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux libwebpmux)
add_library(klimt_formats STATIC src/Sniff.cpp src/Band.cpp src/Png.cpp src/Jpeg.cpp src/Tiff.cpp src/Webp.cpp)
target_include_directories(klimt_formats PUBLIC include)
target_link_libraries(klimt_formats PUBLIC klimt_pixel PRIVATE PNG::PNG JPEG::JPEG TIFF::TIFF PkgConfig::WEBP)
target_compile_features(klimt_formats PUBLIC cxx_std_23)
