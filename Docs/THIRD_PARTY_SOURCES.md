# Eigen source availability

The exact Eigen headers used by the Expo East runtime are included in this
repository at `Plugins/AirSim/Source/AirLib/deps/eigen3`. They are unchanged
from the initial standalone import used by the packaged runtime.

For the existing Linux v0.1.0 download, the same headers are also provided as
[a separate source archive](https://huggingface.co/datasets/IPEC-COMMUNITY/openflyscan/resolve/main/HIL-simulator/linux/v0.1.0/THIRD_PARTY_SOURCES/eigen3.tar.gz).
Download and extract it to obtain the source and original copyright notices.
This attachment does not alter the existing runtime archive. The MPL-2.0 text
is in [Mozilla-Public-License-2.0.txt](../LICENSES/Mozilla-Public-License-2.0.txt).

The packaging script includes `THIRD_PARTY_SOURCES/eigen3.tar.gz` inside newly
assembled packages. Preserve the applicable source and notices when redistributing.
Other dependency licenses are in [third-party notices](../THIRD_PARTY_NOTICES.md).
