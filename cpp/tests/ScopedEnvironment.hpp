#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

class ScopedEnvironment {
private:
    std::string name;
    std::optional<std::string> previous;

public:
    ScopedEnvironment(std::string name, const char* value) : name(std::move(name)) {
        if (const char* current = std::getenv(this->name.c_str()); current != nullptr) {
            previous = current;
        }
        if (value != nullptr) {
            ::setenv(this->name.c_str(), value, 1);
        } else {
            ::unsetenv(this->name.c_str());
        }
    }

    ~ScopedEnvironment() {
        if (previous) {
            ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
};
