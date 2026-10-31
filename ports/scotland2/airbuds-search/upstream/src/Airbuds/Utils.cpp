#include "Airbuds/Utils.hpp"

namespace airbuds {

Image getSmallestImage(const std::vector<Image>& images) {
    Image smallestImage = images.at(0);
    for (const Image& image : images) {
        if (image.width == -1 || image.height == -1) {
            continue;
        }
        if (smallestImage.width == -1 || smallestImage.height == -1 || image.width < smallestImage.width || image.height < smallestImage.height) {
            smallestImage = image;
        }
    }
    return smallestImage;
}

} // namespace airbuds
