#include "ConfigurationStore.hpp"
#include <cassert>
#include <fstream>
#include <sys/stat.h>
static std::string read(const std::filesystem::path& file) {
    std::ifstream input(file,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),{}};
}
int main(int argc,char**argv) {
    assert(argc==2);
    namespace fs=std::filesystem;
    auto root=fs::absolute(argv[1]);fs::create_directories(root);
    auto path=root/"settings.json";
    AirbudsSearch::Configuration first(path.string()); first.Load();
    auto& allocator=first.config.GetAllocator();
    first.config.AddMember("refreshToken","fixture-token",allocator);
    first.config.AddMember("unknownSetting",42,allocator);first.Write();
    auto initial=read(path);assert(initial.find("fixture-token")!=std::string::npos);
    struct stat mode{};assert(!stat(path.c_str(),&mode));assert((mode.st_mode&077)==0);
    AirbudsSearch::Configuration second(path.string());second.Load();
    assert(second.config["unknownSetting"].GetInt()==42);
    second.config["refreshToken"].SetString("replacement",second.config.GetAllocator());second.Write();
    AirbudsSearch::Configuration reopened(path.string());reopened.Load();
    assert(std::string(reopened.config["refreshToken"].GetString())=="replacement");
    assert(reopened.config["unknownSetting"].GetInt()==42);
    {std::ofstream output(path);output<<"malformed existing data";}
    AirbudsSearch::Configuration invalid(path.string());invalid.Load();invalid.Write();
    assert(read(path)=="malformed existing data");
    fs::remove(path);fs::create_symlink(root/"target",path);
    {std::ofstream output(root/"target");output<<"{\"original\":true}";}
    AirbudsSearch::Configuration linked(path.string());linked.Load();linked.Write();
    assert(fs::is_symlink(path));assert(read(root/"target")=="{\"original\":true}");
    fs::remove(root/"target");
    AirbudsSearch::Configuration dangling(path.string());dangling.Load();dangling.Write();
    assert(fs::is_symlink(path));assert(!fs::exists(root/"target"));
    fs::remove(path);
    {std::ofstream output(path);output<<std::string(2*1024*1024+1,'x');}
    AirbudsSearch::Configuration oversized(path.string());oversized.Load();oversized.Write();
    assert(fs::file_size(path)==2*1024*1024+1);
    for(auto const& entry:fs::directory_iterator(root))
        assert(entry.path().filename()=="settings.json" || entry.path().filename()=="target");
    puts("PASS Airbuds token/settings persistence, unknown-field preservation, private file mode, malformed/oversized/symlink preservation and temporary-file cleanup");
}
