# Writes a C++ source with the bytes of a file: cmake -DIN=file -DOUT=out.cpp -DSYMBOL=name -P EmbedBytes.cmake
# defines `extern const unsigned char SYMBOL[]` and `extern const unsigned long SYMBOL_size`.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" chars)
math(EXPR bytes "${chars} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}.tmp" "// generated from ${IN} by cmake/EmbedBytes.cmake\nextern const unsigned char ${SYMBOL}[] = {\n${body}\n};\nextern const unsigned long ${SYMBOL}_size = ${bytes};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
