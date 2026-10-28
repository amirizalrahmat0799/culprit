#include <cstdlib>
#include <ctime>

#include "check.hpp"

int main() {
    // zoned timestamps are shown in local time: pin it so results are the same on every machine
#ifdef _WIN32
    _putenv_s("TZ", "UTC0");
    _tzset();
#else
    setenv("TZ", "UTC0", 1);
    tzset();
#endif
    int run = 0;
    for (const auto& c : check::cases()) {
        int before = check::failures();
        c.fn();
        ++run;
        if (check::failures() != before) std::cerr << "  in test: " << c.name << "\n";
    }
    std::cout << run << " tests, " << check::failures() << " failed checks\n";
    return check::failures() == 0 ? 0 : 1;
}
