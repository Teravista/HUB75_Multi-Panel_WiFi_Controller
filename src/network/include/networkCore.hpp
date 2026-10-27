#pragma once
#include "network/include/NetworkServicesClass.hpp"

// Launches the network worker on Core 1 exactly once.
void StartNetworkCore(NetworkServices& worker);
