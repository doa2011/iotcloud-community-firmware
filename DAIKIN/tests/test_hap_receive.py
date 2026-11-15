"""Compile the production HAP receive frame and exercise TCP/AEAD boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOME = ROOT / 'lib/HomeSpan/src'


class HapReceive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hap-receive-frame-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.build = Path(cls.temp.name)
        source = (HOME / 'HAP.cpp').read_text()
        marker = '__attribute__((noinline)) bool HAPClient::receiveRequest(){'
        start = source.index(marker)
        end = source.index('\n}\n\nvoid HAPClient::processRequest()', start) + 2
        receive = source[start:end]
        (cls.build / 'receive-request.inc').write_text(receive)

        process = source[source.index('void HAPClient::processRequest(){'):]
        process = process.split('\n}', 1)[0]
        assert process.split('{', 1)[1].lstrip().startswith(
            'if(!receiveRequest())return;')
        assert process.index('if(!receiveRequest())return;') < process.index(
            'std::move(request.bytes)')
        fixture = ROOT / 'tests/hub/hap_receive.cpp'
        cls.binary = cls.build / 'hap-receive'
        subprocess.run([
            'c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-fno-pie', '-no-pie', '-I' + str(ROOT / 'include'),
            '-I' + str(cls.build), str(fixture), '-lcrypto',
            '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=20,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_plaintext_partial_and_fragmented_requests(self):
        self.scenario('plaintext')

    def test_encrypted_fragmentation_and_multiple_records(self):
        self.scenario('encrypted')

    def test_bad_tag_and_nonce_exhaustion_close_connection(self):
        self.scenario('failures')


if __name__ == '__main__':
    unittest.main()
