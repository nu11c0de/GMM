#include "Version.h"

// The generated header (from cmake/BuildVersion.h.in) is compiled only into this
// tiny translation unit, so bumping the build number never forces a rebuild of
// the large GUI sources.
#include "BuildVersion.h"

namespace gtamm {

const char* versionString() { return GMM_VERSION_STRING; }
int buildNumber() { return GMM_BUILD_NUMBER; }

}  // namespace gtamm
