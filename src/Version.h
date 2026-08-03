#pragma once

namespace gtamm {

// Human-readable program version, e.g. "1.0002". The build number grows by one
// on every build (see gtamm/BUILD_NUMBER + build.bat / release.bat).
const char* versionString();

// The raw monotonically-increasing build number.
int buildNumber();

}  // namespace gtamm
