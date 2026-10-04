// Where the running program is: the framework's answer to "which file was started, and what is next
// to it", and the rule that turns a path into its directory.
#include <t2d/core/executable.h>

#include <support/test_support.h>

#include <filesystem>
#include <string>

using namespace t2d;

T2D_TEST(a_path_knows_which_directory_it_lives_in) {
    T2D_CHECK_EQ(parent_directory_of("/a/b/c.ecfg"), std::string("/a/b"));
    T2D_CHECK_EQ(parent_directory_of("/a/b"), std::string("/a"));
    T2D_CHECK_EQ(parent_directory_of("/a"), std::string("/"));
    T2D_CHECK_EQ(parent_directory_of("/"), std::string(""));
    // A trailing separator belongs to the directory, not to a name after it.
    T2D_CHECK_EQ(parent_directory_of("/a/b/"), std::string("/a"));
    T2D_CHECK_EQ(parent_directory_of("/a/b//"), std::string("/a"));
    // A bare name is a file in the working directory: it has no directory of its own.
    T2D_CHECK_EQ(parent_directory_of("c.ecfg"), std::string(""));
    T2D_CHECK_EQ(parent_directory_of("a/b"), std::string("a"));
    T2D_CHECK_EQ(parent_directory_of(""), std::string(""));
    T2D_CHECK_EQ(parent_directory_of("///"), std::string(""));
    // A relative path stays relative, and is not quietly turned into something else.
    T2D_CHECK_EQ(parent_directory_of("./x"), std::string("."));
}

T2D_TEST(the_running_executable_knows_where_it_is) {
    const std::string path = executable_path();
    T2D_REQUIRE(!path.empty());
    const std::filesystem::path file(path);
    // The platform's own answer: an absolute path to a file that is there, and the file is this test.
    T2D_CHECK(file.is_absolute());
    T2D_CHECK(std::filesystem::is_regular_file(file));
    T2D_CHECK(std::filesystem::exists(file));
    const std::string name = file.filename().string();
    T2D_CHECK(name.starts_with("test_executable"));

    // The directory is the parent of that path, and it is a directory that exists.
    const std::string directory = executable_directory();
    T2D_REQUIRE(!directory.empty());
    T2D_CHECK_EQ(std::filesystem::path(directory).string(), file.parent_path().string());
    T2D_CHECK(std::filesystem::is_directory(directory));
    // Same directory as the path says: the two answers cannot disagree.
    T2D_CHECK_EQ(parent_directory_of(path), directory);
}

T2D_TEST_MAIN
