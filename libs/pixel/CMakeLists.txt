# libs/pixel -- Klimt's pixels (s006): formats, immutable tiles, sparse tile
# maps, premultiplied float for compositing. GTK-free and dependency-free.
# Its checks live in klimt_selftest (the one gate Scott runs).
add_library(klimt_pixel STATIC src/Pixel.cpp src/Blend.cpp)
target_include_directories(klimt_pixel PUBLIC include)
target_compile_features(klimt_pixel PUBLIC cxx_std_23)
