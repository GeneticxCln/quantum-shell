#pragma once

#include <string>

// The same name as the fixture under src/, which the gate rejects. Under tests/ it is allowed:
// SYSTEM_PROMPT.md permits explicitly named test doubles here.
class MockPowerSource {
public:
    std::string describe() const;
};
