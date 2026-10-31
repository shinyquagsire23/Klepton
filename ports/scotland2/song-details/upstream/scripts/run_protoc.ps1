# make sure protoc is on your PATH (v3.21.11 was used)
$include = "${PSScriptRoot}/../include"
$src = "${PSScriptRoot}/../src"

$songproto = "$include/SongProto.proto"

$songproto_include_cc = "$include/SongProto.pb.cc"
$songproto_src_cc = "$src/SongProto.pb.cc"

& protoc --cpp_out=${include} --proto_path=${include} ${songproto}
move-item ${songproto_include_cc} ${songproto_src_cc} -Force