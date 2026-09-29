# Embeds a text file as a byte array: cmake -DIN=file -DOUT=out.cpp -DNAME=symbol -P EmbedText.cmake
# Hex bytes rather than a string literal, so no character in the file needs escaping.
file(READ "${IN}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
# One array line per line of text, so compiler diagnostics stay readable.
string(REPLACE "0x0a," "0x0a,\n" _bytes "${_bytes}")
file(WRITE "${OUT}.tmp"
    "// Generated from ${IN} by cmake/EmbedText.cmake. Do not edit.\n"
    "#include <cstddef>\n"
    "static const unsigned char kData[] = {\n${_bytes}0};\n"
    "extern const char* const ${NAME} = reinterpret_cast<const char*>(kData);\n"
    "extern const std::size_t ${NAME}Size = sizeof(kData) - 1;\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
