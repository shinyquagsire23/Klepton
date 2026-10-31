#include "Utils.hpp"
#include <cstring>
#include "Data/SongDetailsContainer.hpp"
#include "Data/DataGetter.hpp"
extern "C" __attribute__((visibility("default"))) int sc2_test_songdetails_tls(const char* url,int good){
 auto response=SongDetailsCache::WebUtil::GetAsync(url,20,{}).get();
 return good ? !(response.httpCode==200 && response.curlStatus==CURLE_OK && !response.content.empty()) : !(response.curlStatus!=CURLE_OK && response.content.empty());
}
extern "C" __attribute__((visibility("default"))) int sc2_test_songdetails_hashes(){
 for(auto str:{"", "a", "abc", "zzzz"}){
  auto hash=SongDetailsCache::HexUtil::ToSongHash(str);const unsigned char* bytes=hash;
  for(unsigned i=0;i<20;++i)if(bytes[i])return 1;
 }
 auto hash=SongDetailsCache::HexUtil::ToSongHash("00112233445566778899aabbccddeeff00112233");
 return static_cast<const unsigned char*>(hash)[19]!=0x33;
}

namespace SongDetailsCache {
struct Sc2BoundaryTests {
 static int run() {
  if (SongDetailsContainer::songs) return 1;
  Structs::SongDetailsV3 data;
  auto song=data.add_songs(); song->set_mapid(7); song->set_songname("fixture");
  auto rejected=[&] { SongDetailsContainer::Process(data,true); return !SongDetailsContainer::songs; };
  if (!rejected()) return 2; // missing packed hashes
  data.set_songhashes(std::string(20,'x'));
  for(int i=0;i<65;++i)data.add_taglist("tag");
  if (!rejected()) return 3;
  data.clear_taglist();
  for(int i=0;i<256;++i)song->add_difficulties();
  if (!rejected()) return 4;
  song->clear_difficulties(); data.set_scrapeendedunix(17);
  SongDetailsContainer::Process(data,true);
  auto saved=SongDetailsContainer::songs;
  if (!saved || saved->size()!=1 || SongDetailsContainer::hashBytes->size()!=1 || SongDetailsContainer::keys->at(0)!=7) return 5;
  const uint8_t* bytes=SongDetailsContainer::hashBytes->at(0);
  for(int i=0;i<20;++i)if(bytes[i]!='x')return 6;
  data.set_songhashes("short");data.set_scrapeendedunix(999);
  SongDetailsContainer::Process(data,true);
  if(SongDetailsContainer::songs!=saved || SongDetailsContainer::scrapeEndedTimeUnix.time_since_epoch().count()!=17)return 7;
  std::vector<uint8_t> output;
  if(DataGetter::DecompressBrotli(output,"") || DataGetter::DecompressBrotli(output,"corrupted brotli"))return 8;
  return 0;
 }
};
}
extern "C" __attribute__((visibility("default"))) int sc2_test_songdetails_payloads() {
 return SongDetailsCache::Sc2BoundaryTests::run();
}
