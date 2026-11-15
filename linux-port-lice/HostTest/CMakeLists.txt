# The controlled test host: the SDK's editorhost sample
# (public.sdk/samples/vst-hosting/editorhost, BSD-licensed) with local
# includes and its Linux run loop corrected — see README.md. The SDK itself
# is never edited.
#
# Built only with -DOXO_HOST_TEST=ON; not part of a release.

set(OXO_HOSTTEST_ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_executable(oxo_editorhost
    "${OXO_HOSTTEST_ROOT}/src/source/editorhost.cpp"
    "${OXO_HOSTTEST_ROOT}/src/source/usediids.cpp"
    "${OXO_HOSTTEST_ROOT}/src/source/platform/linux/platform.cpp"
    "${OXO_HOSTTEST_ROOT}/src/source/platform/linux/runloop.cpp"
    "${OXO_HOSTTEST_ROOT}/src/source/platform/linux/window.cpp"
    "${OXO_SDK}/public.sdk/source/vst/hosting/module_linux.cpp"
    "${OXO_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp"
)
target_compile_features(oxo_editorhost PRIVATE cxx_std_17)
# The fork's sources include each other with local paths ("editorhost.h",
# "platform/..."): src/source is the include root.
target_include_directories(oxo_editorhost PRIVATE
    "${OXO_HOSTTEST_ROOT}/src/source"
    "${OXO_SDK}"
)
find_package(X11 REQUIRED)
target_link_libraries(oxo_editorhost PRIVATE sdk_hosting X11::X11)
