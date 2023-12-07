#include "stdafx.hpp"
#include "common_fns.hpp"

bool misc_options::save_to_disk() const noexcept
{
    try {
        std::ofstream out("data/misc_options.txt", std::ios::binary);

        if (!out) {
            return false;
        }

        static_assert(s8(1) == s8(true));
        static_assert(s8(0) == s8(false));


        return true;
    }
    catch (...) {
        return false;
    }
}

bool misc_options::load_from_disk() noexcept
{
    try {
        std::ifstream in("data/misc_options.txt", std::ios::binary);
        if (!in) {
            return false;
        }

        static_assert(s8(1) == s8(true));
        static_assert(s8(0) == s8(false));

        std::string what = {};
        what.reserve(100);
        // char bit_ch = 0;


        return true;
    }
    catch (...) {
        return false;
    }
}
