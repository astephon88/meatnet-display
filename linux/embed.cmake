# cmake -DIN=www/index.html -DOUT=index_html.cpp -P embed.cmake
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR len "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "#include <cstddef>\nextern const char index_html[] = {${bytes}0};\nextern const size_t index_html_len = ${len};\n")
