# Writes a C++ file holding one file as bytes (the web viewer's page), so culprit stays a single program.
#
#   cmake -DIN=<file> -DOUT=<file.cpp> -DSYMBOL=<name> -P embed_file.cmake
#
# Defines `extern const unsigned char <SYMBOL>[]` and `extern const size_t <SYMBOL>_size` in namespace culprit.

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" chars)
math(EXPR size "${chars} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n  " bytes "${bytes}")
get_filename_component(name "${IN}" NAME)

set(content "// Generated from ${name} by cmake/embed_file.cmake. Don't edit.
#include <cstddef>

namespace culprit {

extern const unsigned char ${SYMBOL}[] = {
  ${bytes}0x00};
extern const size_t ${SYMBOL}_size = ${size};

}  // namespace culprit
")
file(WRITE "${OUT}.tmp" "${content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
