# CMake generated Testfile for 
# Source directory: /Users/lunks/code/stray-dlss
# Build directory: /Users/lunks/code/stray-dlss/build-local
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[core]=] "/Users/lunks/code/stray-dlss/build-local/stray_dlss_tests")
set_tests_properties([=[core]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/lunks/code/stray-dlss/CMakeLists.txt;149;add_test;/Users/lunks/code/stray-dlss/CMakeLists.txt;0;")
subdirs("_deps/doctest-build")
