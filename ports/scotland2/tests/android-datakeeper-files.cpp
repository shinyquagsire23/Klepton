// Diagnostic calls the original game's save APIs through the installed mod hooks.
#include "main.hpp"
#include "config.hpp"
#include "System/IO/File.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
extern "C" void setup(CModInfo*);
static std::string read_file(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
extern "C" __attribute__((visibility("default"))) int sc2_test_datakeeper_files(){
    auto root=std::filesystem::canonical(modloader::get_external_dir());
    auto backup=std::filesystem::path(getConfig().backupPath.GetValue());
    auto main=root/"sc2-save-probe.json";auto temp=root/"sc2-save-probe.tmp";
    auto local=root.parent_path()/LOCAL_FILES_DIR/"sc2-local-save-probe.json";
    std::filesystem::create_directories(local.parent_path());
    System::IO::File::WriteAllText(main.string(),"SC2_ORIGINAL_WRITE_SAVE");
    if(read_file(main)!="SC2_ORIGINAL_WRITE_SAVE"||read_file(backup/main.filename())!="SC2_ORIGINAL_WRITE_SAVE")return 1;
    System::IO::File::WriteAllText(temp.string(),"TEMP_NOT_BACKED_UP");
    if(std::filesystem::exists(backup/temp.filename()))return 2;
    System::IO::File::Replace(temp.string(),main.string(),nullptr,true);
    if(read_file(main)!="TEMP_NOT_BACKED_UP"||read_file(backup/main.filename())!="TEMP_NOT_BACKED_UP")return 5;
    System::IO::File::WriteAllText(local.string(),"SC2_ORIGINAL_LOCAL_SAVE");
    if(read_file(backup/LOCAL_FILES_DIR/local.filename())!="SC2_ORIGINAL_LOCAL_SAVE")return 3;
    std::filesystem::remove(main);
    std::filesystem::remove(local);
    CModInfo info{};setup(&info); // same original startup restore path, isolated host fixture only.
    if(read_file(main)!="TEMP_NOT_BACKED_UP"||read_file(local)!="SC2_ORIGINAL_LOCAL_SAVE")return 4;
    for(const auto& p:{main,temp,local,backup/main.filename(),backup/LOCAL_FILES_DIR/local.filename()})std::filesystem::remove(p);
    std::fprintf(stderr,"DATAKEEPER_BACKUP_RESTORE_OK original WriteAllText and File.Replace hooks copy normal/local saves, excludes tmp and original setup restores backups\n");return 0;
}
