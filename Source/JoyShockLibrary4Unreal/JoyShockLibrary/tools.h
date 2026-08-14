// tools.h - The two byte-fiddling helpers the parsers still use.
//
// Inherited from JibbSmart's library, which had a fuller set: signed/unsigned conversions, a bit-mask
// builder and four variations on a hex dump. Only these two ever had a caller here, so the rest is gone
// rather than left as a menu of functions nobody picked from.
#pragma once

#include <cstdint>
#include <cstring>

// Reinterprets a controller's raw 16-bit sensor word as signed, without the implementation-defined
// behaviour of a cast. Used by every family's report parser.
int16_t uint16_to_int16(uint16_t a);

// Prints a report as space-separated hex bytes. A debugging aid: this is how a new controller's report
// layout gets read off the wire in the first place.
void hex_dump(unsigned char *buf, int len);
