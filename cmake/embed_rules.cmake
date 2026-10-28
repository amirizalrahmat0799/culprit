# Writes a C++ file holding every rules/*.toml file as bytes, so culprit's own rules are part of the
# program and nothing has to be installed next to it.
#
#   cmake -DDIR=<rules folder> -DOUT=<file.cpp> -P embed_rules.cmake

file(GLOB files "${DIR}/*.toml")
list(SORT files)

set(arrays "")
set(table "")
set(count 0)
foreach(f IN LISTS files)
  file(READ "${f}" hex HEX)
  string(LENGTH "${hex}" chars)
  math(EXPR size "${chars} / 2")
  # 0x.. bytes, 24 to a line
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n  " bytes "${bytes}")
  get_filename_component(name "${f}" NAME)
  string(APPEND arrays "static const unsigned char file${count}[] = {\n  ${bytes}};\n")
  string(APPEND table "    {\"${name}\", file${count}, ${size}},\n")
  math(EXPR count "${count} + 1")
endforeach()
if(count EQUAL 0)
  set(table "    {\"\", nullptr, 0},\n")
endif()

set(content "// Generated from rules/*.toml by cmake/embed_rules.cmake. Don't edit.
#include <cstddef>

namespace culprit {

struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    size_t size;
};

${arrays}
extern const EmbeddedFile kBuiltinRules[] = {
${table}};
extern const size_t kBuiltinRuleCount = ${count};

}  // namespace culprit
")
file(WRITE "${OUT}.tmp" "${content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
