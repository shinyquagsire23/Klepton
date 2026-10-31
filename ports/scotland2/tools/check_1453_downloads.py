#!/usr/bin/env python3
"""Test real guest download clients with local TLS/ZIP fixtures, without game boot."""
import argparse,http.server,io,json,os,ssl,struct,subprocess,sys,threading,zipfile
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]

def private_symbol(path,name):
    sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf,cstr
    elf=Elf(str(path));found=set()
    for table in elf.sh:
        if table[1] not in (2,11):continue
        strings=elf.sh[table[6]][4]
        for at in range(table[4],table[4]+table[5],24):
            n,info,other,index,address,size=struct.unpack_from('<IBBHQQ',elf.data,at)
            if index and info&15==2 and cstr(elf.data,strings+n)==name and size>=4:found.add(address)
    if len(found)!=1:raise ValueError('Ambiguous/missing production helper: '+name)
    return found.pop()

def zip_bytes(entries):
    buffer=io.BytesIO()
    with zipfile.ZipFile(buffer,'w',zipfile.ZIP_DEFLATED) as output:
        for name,data,mode in entries:
            item=zipfile.ZipInfo(name);item.external_attr=mode<<16;item.compress_type=zipfile.ZIP_DEFLATED
            output.writestr(item,data)
    return buffer.getvalue()

class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body=b'{"fixture":"verified local TLS","padding":"'+b'x'*8192+b'"}'
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def log_message(self,*args):pass

def check(work,imports,legacy):
    validated=json.loads((imports/'validation.json').read_text())
    if validated['returnCode'] or 'HOST_IMPORTS_AND_CONSTRUCTORS_OK ' not in (imports/'run.log').read_text():
        raise ValueError('Require a completed strict import check')
    for row in validated['modules']:
        if bootstrap.digest(work/'build/libs'/row['name'])!=row['sha256']:raise ValueError('Changed module: '+row['name'])
    run=lambda a,**kw:subprocess.run([str(x) for x in a],check=True,cwd=ROOT,**kw)
    run(['make','SC2_BUILD_MODE=release','build/t_scotland2_il2cpp'])
    for attempt in range(100):
        folder=work/'build'/('mac-download-check'+('' if not attempt else '-'+str(attempt+1)))
        if not folder.exists():break
    else:raise ValueError('Too many retained download checks')
    folder.mkdir()
    for name in ['Frameworks','Scotland2Metadata']:run(['cp','-cR',imports/name,folder/name])
    il2cpp=work/'sources/libil2cpp/il2cpp';tools=work/'toolchains/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin'
    includes=[work/'build/sdk',work/'sources/bs-cordl/include',il2cpp/'libil2cpp',
              il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
              work/'work-sources/fmt/include',legacy/'utfcpp/source',legacy/'protobuf/src',
              work/'work-sources/song-details/include',work/'work-sources/song-details/shared',
              work/'work-sources/songdownloader/include']
    fixture=folder/'Scotland2Metadata/shims/libsc2_1453_downloads.so'
    run([tools/'aarch64-linux-android26-clang++','-shared','-std=c++23','-O1','-fPIC','-fvisibility=hidden',
         '-fdeclspec','-DANDROID','-DUNITY_6','-DUNITY_6_3','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP',
         '-DFMT_HEADER_ONLY','-DWEBUTILS_NO_BSML','-DSC2_DIAGNOSTICS=0',
         '-include',ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp',
         *[v for path in includes for v in ['-I',path]],ROOT/'ports/scotland2/tests/android-1453-downloads.cpp',
         '-L'+str(work/'build/libs'),'-l:libweb-utils.so','-l:libbeatsaverplusplus.so','-l:libpaper2_scotland2.so',
         '-l:libsl2_abi.so','-llog','-ldl','-lz','-static-libstdc++','-Wl,-z,max-page-size=16384',
         '-Wl,--exclude-libs,ALL','-Wl,-soname,'+fixture.name,'-o',fixture])
    translated=folder/'Frameworks/libsc2_1453_downloads.dylib'
    run([ROOT/'build/klepton-ld',fixture,'--platform','macos','--install-name','@rpath/'+translated.name,'-o',translated,'--quiet'])
    run(['codesign','--force','--sign','-',translated])
    helpers=[('libsongdownloader.so','_ZN8WebUtils3GetENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEElRS6_'),
             ('libsongdetails.so','_ZN16SongDetailsCache7WebUtil8GetAsyncENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEEjRKNS1_13unordered_mapINS1_12basic_stringIcS4_NS1_9allocatorIcEEEESA_NS1_4hashISA_EENS1_8equal_toISA_EENS8_INS1_4pairIKSA_SA_EEEEEE'),
             ('libsongdownloader.so','zip_stream_extract')]
    debug_images={}
    for module,directory in [('libsongdownloader.so','songdownloader'),('libsongdetails.so','song-details')]:
        debug=work/'build'/directory/(Path(module).stem+'.debug.so')
        derivative=folder/module
        run([tools/'llvm-objcopy','--strip-all',debug,derivative])
        if bootstrap.digest(derivative)!=bootstrap.digest(work/'build/libs'/module):
            raise ValueError('Private symbols differ from the shipping production module: '+module)
        debug_images[module]=debug
    addresses=[{'module':name,'symbol':symbol,'vaddr':hex(private_symbol(debug_images[name],symbol)),
                'privateSymbolsSha256':bootstrap.digest(debug_images[name])} for name,symbol in helpers]
    profile=folder/'private-targets.txt';profile.write_text(''.join(r['module']+' '+r['vaddr']+'\n' for r in addresses))
    good=zip_bytes([('payload.txt',b'SCOTLAND2_REAL_ZIP_DEFLATE\n'*100,0o100644)])
    good_path=folder/'good.zip';good_path.write_bytes(good)
    bad=[zip_bytes([(name,b'bad',mode)]) for name,mode in [('..'+'/escape',0o100644),('/absolute',0o100644),
         ('x/../../escape',0o100644),('a\\b',0o100644),('C:bad',0o100644),('link',0o120777),('pipe',0o010644),('./file',0o100644)]]
    bad += [zip_bytes([('/'.join(['nested']*40)+'/file',b'bad',0o100644)]),good[:-10],b'not zip',
            zip_bytes([('a',b'x',0o100644),('A',b'y',0o100644)]),zip_bytes([('bomb',b'0'*2000000,0o100644)])]
    paths=[]
    for index,data in enumerate(bad):
        path=folder/('bad-'+str(index)+'.zip');path.write_bytes(data);paths.append(path)
    bad_paths=folder/'bad-zips.txt';bad_paths.write_text(''.join(str(path)+'\n' for path in paths))
    cert_config=folder/'fixture-cert.cnf'
    cert_config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n')
    servers=[]
    try:
        for name in ['trusted','untrusted']:
            key=folder/(name+'-fixture.key');cert=folder/(name+'-fixture.pem')
            with (folder/(name+'-openssl.log')).open('w') as log:
                run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-config',cert_config,
                     '-keyout',key,'-out',cert],stdout=log,stderr=subprocess.STDOUT)
            key.chmod(0o600)
            server=http.server.ThreadingHTTPServer(('127.0.0.1',0),FixtureHandler)
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
            server.socket=context.wrap_socket(server.socket,server_side=True)
            threading.Thread(target=server.serve_forever,daemon=True).start();servers.append(server)
        userdata=folder/'userdata';userdata.mkdir()
        env={k:v for k,v in os.environ.items() if not k.startswith(('KL_','SC2_')) and not k.upper().endswith('_PROXY')}
        env.update(KL_SCOTLAND2='1',KL_SCOTLAND2_ROOT=str(folder/'Scotland2Metadata'),
                   KL_DYLIB_DIR=str(folder/'Frameworks'),KL_FILES_DIR=str(userdata),KL_GUEST_JIT='0',
                   KL_SCOTLAND2_ABI='1',KL_SCOTLAND2_NATIVE_GATES='1',KL_SCOTLAND2_FLAMINGO='1',
                   KL_SCOTLAND2_FLAMINGO_LIBRARY='libflamingo.so',KL_SCOTLAND2_CA_BUNDLE=str(folder/'trusted-fixture.pem'),
                   SC2_1453_DOWNLOAD_PROBE=str(fixture),SC2_1453_PRIVATE_TARGETS=str(profile),SC2_1453_BAD_ZIPS=str(bad_paths),
                   SC2_TEST_ZIP_PATH=str(good_path),SC2_1453_GOOD_TLS='https://localhost:'+str(servers[0].server_port),
                   SC2_1453_BAD_TLS='https://localhost:'+str(servers[1].server_port),
                   SC2_1453_BAD_HOST='https://127.0.0.1:'+str(servers[0].server_port),NO_PROXY='localhost,127.0.0.1')
        with (folder/'run.log').open('w') as log:
            try:
                result=subprocess.run([str(ROOT/'build/t_scotland2_il2cpp'),str(ROOT/'beatsaber140/lib/arm64-v8a/libil2cpp.so'),
                    str(ROOT/'beatsaber140/assets/bin/Data/Managed')],env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=180)
                code=result.returncode
            except subprocess.TimeoutExpired:code=124
    finally:
        for server in servers:server.shutdown();server.server_close()
    markers=[line for line in (folder/'run.log').read_text().splitlines() if line.startswith('DOWNLOAD_BOUNDARIES_1453_OK ')]
    passed=code==0 and len(markers)==1
    (folder/'validation.json').write_text(json.dumps({'passed':passed,'returnCode':code,'modules':validated['modules'],
        'privateProductionHelpers':addresses,'fixtureSha256':bootstrap.digest(fixture),
        'harnessSha256':bootstrap.digest(ROOT/'build/t_scotland2_il2cpp'),
        'scope':'Local TLS fixtures exercise original WebUtils/SongDetails/SongDownloader clients and production ZIP paths; no authenticated API, game startup or headset test'},indent=2)+'\n')
    if not passed:raise RuntimeError('Download boundary check failed; inspect '+str(folder/'run.log'))
    print(markers[0])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--imports',type=Path,required=True);p.add_argument('--legacy-dependencies',type=Path,required=True)
    a=p.parse_args();check(a.workspace.resolve(),a.imports.resolve(),a.legacy_dependencies.resolve())
