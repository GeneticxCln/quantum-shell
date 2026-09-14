#pragma once

#include <string>

// A collaborator named after a test double. Outside tests/ this is rejected, because a shipped
// interface whose only implementation is a double is not an implementation.
class MockPowerSource {
public:
    std::string describe() const;
};
