import importlib.util
from pathlib import Path
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/doctor.py'
spec = importlib.util.spec_from_file_location('doctor', SCRIPT)
doctor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(doctor)

SAMPLE = """Devices:
========
GPU0:
\tapiVersion         = 1.4.351
\tdriverVersion      = 615.78.8.0
\tvendorID           = 0x10de
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = NVIDIA GeForce RTX 4050 Laptop GPU
\tdriverName         = NVIDIA
GPU1:
\tapiVersion         = 1.4.354
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 23.1.1, 256 bits)
\tdriverName         = llvmpipe
"""


class DoctorTests(unittest.TestCase):
    def test_vendor_names(self):
        self.assertEqual(doctor.vendor('NVIDIA GeForce RTX 4050 Laptop GPU'), 'nvidia')
        self.assertEqual(doctor.vendor('AMD Radeon RX 6600 (RADV NAVI23)'), 'amd')
        self.assertEqual(doctor.vendor('Intel(R) Graphics (RPL-S)'), 'intel')
        self.assertEqual(doctor.vendor('Something else'), 'other')

    def test_parses_vulkaninfo_summary(self):
        with mock.patch.object(doctor, 'run', return_value=SAMPLE):
            devices, have = doctor.vulkan_devices()
        self.assertTrue(have)
        self.assertEqual([d['name'] for d in devices][0], 'NVIDIA GeForce RTX 4050 Laptop GPU')
        self.assertEqual(devices[0]['api'], (1, 4))
        self.assertEqual(devices[1]['type'], 'cpu')

    def test_software_only_vulkan_is_a_problem(self):
        only_cpu = SAMPLE.split('GPU1:')[0].split('GPU0:')[0] + 'GPU0:\n\tapiVersion = 1.4.1\n\tdeviceType = PHYSICAL_DEVICE_TYPE_CPU\n\tdeviceName = llvmpipe\n'
        with mock.patch.object(doctor, 'run', return_value=only_cpu), mock.patch.object(doctor, 'memory', return_value=(16.0, 16.0)):
            facts = doctor.check()
        self.assertTrue(any('llvmpipe' in p for p in facts['problems']))

    def test_old_vulkan_is_a_problem_and_amd_gets_a_note(self):
        amd = 'GPU0:\n\tapiVersion = 1.1.200\n\tdeviceType = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU\n\tdeviceName = AMD Radeon RX 6600\n'
        with mock.patch.object(doctor, 'run', return_value=amd), mock.patch.object(doctor, 'memory', return_value=(16.0, 16.0)):
            facts = doctor.check()
        self.assertTrue(any('1.2 or newer' in p for p in facts['problems']))
        self.assertTrue(any('untested' in n for n in facts['notes']))

    def test_missing_vulkaninfo_is_only_a_warning(self):
        with mock.patch.object(doctor, 'run', return_value=''), mock.patch.object(doctor, 'memory', return_value=(16.0, 16.0)):
            facts = doctor.check()
        self.assertFalse(any('Vulkan' in p for p in facts['problems']))
        self.assertTrue(any('vulkaninfo' in w for w in facts['warnings']))


if __name__ == '__main__':
    unittest.main()
