"""Exercise the production G12 sensor owner against deterministic bus/NVS edges."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from DAIKIN.tests.arduinojson import codec_source
ROOT = Path(__file__).resolve().parents[2]
class SensorRuntime(unittest.TestCase):
    def test_owner(self):
        with tempfile.TemporaryDirectory(prefix='g12-sensor-') as directory:
            temp=Path(directory)
            (temp/'esp_sntp.h').write_text('#pragma once\n#include <sys/time.h>\nstatic inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval*)){}\n')
            for name in ('sensor_runtime','sensor_compensation','frames'):
                output=temp/name
                subprocess.run(['g++','-std=c++11','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-I'+str(temp),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(codec_source()),str(ROOT/'DAIKIN/tests'/f'{name}.cpp'),'-o',str(output)],check=True)
                subprocess.run([str(output)],check=True)
