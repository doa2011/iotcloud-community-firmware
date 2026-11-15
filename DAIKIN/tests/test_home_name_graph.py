"""Production HomeKit name constructors/callbacks and dynamic synchronization."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[2]
class HomeNameGraph(unittest.TestCase):
    def test_configuration_refresh_persists_and_announces_unchanged_hash(self):
        source=(ROOT/'DAIKIN/lib/HomeSpan/src/HomeSpan.cpp').read_text()
        function='boolean Span::updateDatabase('+source.split('boolean Span::updateDatabase(',1)[1].split('\n///////////////////////////////',1)[0]
        fixture=r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
using boolean=bool;
enum {GET_META=1,GET_PERMS=2,GET_TYPE=4,GET_DESC=8};
struct {unsigned char hash[48]={};void flush(){}const void *getHash(){return hash;}}hapOut;
struct SpanService{virtual void loop(){}};
struct Accessory{std::vector<SpanService*> Services;};
static unsigned broadcasts=0,lastConfig=0;
static void mdns_service_txt_item_set(const char*,const char*,const char*,const char *value){++broadcasts;lastConfig=unsigned(std::stoi(value));}
struct Span{
  struct Config{unsigned char hashCode[48]={};unsigned configNumber=1;}hapConfig;
  struct NVS{unsigned writes=0;bool good=true;Config saved;bool writeBlob(int,const char*,const void *value,size_t size){assert(size==sizeof(Config));++writes;if(!good)return false;saved=*static_cast<const Config*>(value);return true;}}iotcloudNVS;
  int hapNVS=0;std::vector<Accessory*> Accessories;std::vector<SpanService*> Loops;
  void printfAttributes(unsigned flags){assert(flags==(GET_META|GET_PERMS|GET_TYPE|GET_DESC));}
  boolean updateDatabase(boolean=true,boolean=false);
}homeSpan;
'''
        fixture=fixture.replace('#include <vector>','#include <vector>\n#include <string>')+function+r'''
int main(){
  auto &span=homeSpan;
  assert(!span.updateDatabase()&&span.iotcloudNVS.writes==0&&broadcasts==0);
  assert(span.updateDatabase(true,true)&&span.hapConfig.configNumber==2);
  assert(span.iotcloudNVS.writes==1&&span.iotcloudNVS.saved.configNumber==2&&lastConfig==2&&broadcasts==1);
  assert(!span.updateDatabase()&&span.iotcloudNVS.writes==1&&broadcasts==1);
  span.hapConfig.configNumber=65535;
  assert(span.updateDatabase(false,true)&&span.hapConfig.configNumber==1);
  assert(span.iotcloudNVS.saved.configNumber==1&&broadcasts==1);
  hapOut.hash[0]=1;
  assert(span.updateDatabase()&&span.iotcloudNVS.saved.hashCode[0]==1&&lastConfig==2&&broadcasts==2);
  span.iotcloudNVS.good=false;
  assert(!span.updateDatabase(true,true)&&broadcasts==2);
}
'''
        with tempfile.TemporaryDirectory(prefix='home-config-refresh-') as temporary:
            build=Path(temporary);cpp=build/'refresh.cpp';binary=build/'test'
            cpp.write_text(fixture)
            subprocess.run(['c++','-std=c++11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=15,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

    def test_production_name_callbacks_preserve_paired_identifiers(self):
        source=(ROOT/'DAIKIN/src/home.inc').read_text()
        with tempfile.TemporaryDirectory(prefix='home-name-graph-') as temporary:
            build=Path(temporary)
            (build/'home-name-graph.inc').write_text('static bool homeAwaitControl(){'+source.split('static bool homeAwaitControl(){',1)[1].split('static bool homeMemoryReady(){',1)[0])
            (build/'home-graph-build.inc').write_text('homeAccessory(1,'+source.split('homeAccessory(1,',1)[1].split('if(!homeSpan.iotcloudInitialize())',1)[0])
            binary=build/'test'
            subprocess.run(['c++','-std=c++11','-g','-O1','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-Wno-sign-compare','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-fno-pie','-no-pie','-I'+str(build),'-I'+str(ROOT/'DAIKIN/include'),
                '-I'+str(codec_source()),str(ROOT/'DAIKIN/tests/hub/home_name_graph.cpp'),
                '-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=15,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
