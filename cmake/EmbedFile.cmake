# Embeds a binary file as a C++ byte array: cmake -DIN=<file> -DOUT=<inc> -DNAME=<identifier> -P EmbedFile.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// Generated from ${IN} - do not edit by hand.\nalignas(4) static const unsigned char ${NAME}[${size}] = {\n${bytes}\n};\n")