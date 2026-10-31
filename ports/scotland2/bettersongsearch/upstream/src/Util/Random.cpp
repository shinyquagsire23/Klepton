#include "Util/Random.hpp"

#include <random>

namespace BetterSongSearch::Util {
    // Generate random number
    int random(int min, int max)  // range : [min, max]
    {
        static std::random_device rd;
        static std::mt19937 generator(rd());

        std::uniform_int_distribution<int> distribution(min, max);
        return distribution(generator);
    }
}  // namespace BetterSongSearch::Util
