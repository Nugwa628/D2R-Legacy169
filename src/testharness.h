// Test harness of test builds (cmake -DLEGACY169_TEST_HARNESS=ON, src/testharness.cpp): background input for unattended
// runs on plain D2R. Release builds do not compile it in.
#pragma once

#include <cstddef>

// Installs the harness when legacy169-testharness.txt sits next to the DLL; `report` gets one line for the log.
auto InstallTestHarness(char* report, size_t reportSize) noexcept -> bool;
