#include "photon/photon.h"

#ifndef PHOTON_VERSION
#define PHOTON_VERSION "0.0.0"
#endif

namespace photon
{
const char *version()
{
  return PHOTON_VERSION;
}
} // namespace photon
