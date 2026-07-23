// The single translation unit that instantiates the vendored optparse
// implementation; everything else just includes optparse.h.  Kept in its own
// target so third-party code isn't held to this project's warning settings.
#define OPTPARSE_IMPLEMENTATION
#include "optparse.h"
