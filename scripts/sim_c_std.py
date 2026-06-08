"""Pin C (not C++) compilation to gnu17 for the native simulator build.

GCC 16 defaults C to gnu23 (C23), which makes `bool`, `true`, and `false`
keywords. Third-party C sources like ricmoo/QRCode's qrcode.c still do
`typedef unsigned char bool;` (guarded only by `#ifndef __cplusplus`), which is
a hard error under C23. Adding -std=gnu17 to CFLAGS only (CXXFLAGS keeps the
project's gnu++2a) restores the pre-C23 behaviour for C TUs without touching C++.
"""

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

env.Append(CFLAGS=["-std=gnu17"])
