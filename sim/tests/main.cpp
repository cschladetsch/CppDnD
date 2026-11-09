// dnd_tests: the unit and integration tests (GoogleTest). Game content is loaded once for the whole run.
#include <gtest/gtest.h>

#include <iostream>

#include "dnd/Content.hpp"

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    try {
        dnd::Content::Load(DND_DATA_DIR);
    } catch (const std::exception& e) {
        std::cerr << "cannot load content from '" << DND_DATA_DIR << "': " << e.what() << "\n";
        return 2;
    }
    return RUN_ALL_TESTS();
}
