#include "safe-save.hpp"
#include <cassert>
#include <fstream>
#include <string>
int main(int argc,char**argv){
    assert(argc==2);namespace fs=std::filesystem;fs::path root=argv[1];fs::create_directories(root);
    auto source=root/"source",backup=root/"backup/save";
    {std::ofstream f(source);f<<"original-save";}
    fs::create_directories(backup.parent_path());
    auto collision=backup.parent_path()/(".sc2-save-"+std::to_string(getpid())+"-0");
    auto collisionLink=backup.parent_path()/(".sc2-save-"+std::to_string(getpid())+"-1");
    {std::ofstream f(collision);f<<"retained collision";}
    fs::create_symlink(source,collisionLink);
    assert(sc2_save::atomic_copy(source,backup));
    {std::ifstream f(collision);std::string content;std::getline(f,content);assert(content=="retained collision");}
    assert(fs::is_symlink(collisionLink));fs::remove(collision);fs::remove(collisionLink);
    fs::remove(source);assert(sc2_save::should_restore(backup,source));assert(sc2_save::atomic_copy(backup,source));
    fs::last_write_time(source,fs::last_write_time(backup)+std::chrono::seconds(5));assert(!sc2_save::should_restore(backup,source));
    auto link=root/"link";fs::create_symlink(source,link);assert(!sc2_save::atomic_copy(link,backup));assert(!sc2_save::should_restore(backup,link));
    assert(!sc2_save::atomic_copy(root/"missing",backup));
    std::ifstream f(backup);std::string data;f>>data;assert(data=="original-save");
    for(auto const& e:fs::directory_iterator(backup.parent_path()))assert(e.path().filename()=="save");
    puts("PASS atomic save backup/recovery, newer live save preserved, missing/link sources rejected without damaging backup, no stale temporary files");
}
