#include "../ports/scotland2/src/search-port-support.hpp"
#include <cassert>
#include <iostream>

int main() {
    using sc2_search::base64_url_decode;
    assert(base64_url_decode("eyJ1c3IiOiJmaXh0dXJlIn0") == "{\"usr\":\"fixture\"}");
    assert(base64_url_decode("Zg==") == "f");
    assert(base64_url_decode("Zm8=") == "fo");
    assert(base64_url_decode("Zm9v") == "foo");
    assert(base64_url_decode("Zm9vYmFy") == "foobar");
    assert(base64_url_decode("-_8") == std::string("\xfb\xff", 2));
    assert(base64_url_decode("+/8=") == std::string("\xfb\xff", 2));
    for (const auto* bad : {"A", "Zg=", "Zg===", "=Zg=", "Z g==", "Zg!!", "Zh=="})
        assert(base64_url_decode(bad).empty());
    assert(base64_url_decode(std::string(16385, 'A')).empty());
    assert(base64_url_decode(std::string(16384, 'A')).size() == 12288);
    std::cout << "PASS bounded original-client JWT payload decoding, padding, URL alphabet and invalid inputs\n";
}
