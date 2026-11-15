"""Production HTTP response and mutation MessagePack paths."""
from pathlib import Path
import subprocess,tempfile,unittest
from DAIKIN.tests.arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[2]
class HttpResponse(unittest.TestCase):
 def test_production_emitter_object_array_empty_and_error(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);source=(ROOT/'DAIKIN/src/http.inc').read_text();(p/'response.inc').write_text(source[:source.index('static bool authorized(')])
   start=source.index('  if(body.length()&&!deviceCommand){')
   end=source.index('  if(!captureWebAuthorization(req))',start)
   mutation=source[start:end]
   self.assertEqual(mutation.count('DynamicJsonDocument decoded('),1)
   (p/'mutation.inc').write_text('static esp_err_t decodeHubMutation(String &body,bool deviceCommand,httpd_req_t *req){\n'+mutation+'\n return ESP_OK;\n}\n')
   for variant,defines in [('full/G3',[]),('hub',['-DIOTCLOUD_HUB=1'])]:
    with self.subTest(variant=variant):
     exe=p/variant.replace('/','-')
     subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',*defines,'-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(p),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec_source()),str(ROOT/'DAIKIN/tests/hub/http_response.cpp'),'-o',str(exe)],check=True)
     subprocess.run([str(exe)],check=True)
