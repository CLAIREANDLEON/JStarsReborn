"""Synthetic instructions only; no game dump needed."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools' / 're'))
from timing_candidates import scan, rip_target, TIMING


def synthetic_module():
    image = bytearray(0x140)
    image[:0x60] = b'\x90' * 0x60
    # call wrapper; wrapper tail-jumps to a relocated import trampoline.
    image[0:5] = b'\xe8' + struct.pack('<i', 0x1010 - 0x1005)
    image[0x10:0x15] = b'\xe9' + struct.pack('<i', 0x1020 - 0x1015)
    image[0x20:0x26] = b'\xff\x25' + struct.pack('<i', 0x2010 - 0x1026)
    image[0x30:0x38] = b'\xf3\x0f\x10\x05' + struct.pack('<i', 0x2000 - 0x1038)
    struct.pack_into('<f', image, 0x100, 1 / 30)
    struct.pack_into('<f', image, 0x104, 1 / 60)  # unreferenced, must not be reported
    return {'elf': image, 'ph': [
        {'type': 1, 'flags': 5, 'offset': 0, 'vaddr': 0x1000, 'filesz': 0x60},
        {'type': 1, 'flags': 6, 'offset': 0x100, 'vaddr': 0x2000, 'filesz': 0x40}],
        'relocs': [(0x2010, 7, 0, 0)],
        'symbols': [{'identity': ('clock_nid', ('libkernel', 1), None)}],
        'sha256': 'synthetic'}


class TimingCandidatesTest(unittest.TestCase):
    def test_tail_wrapper_and_its_caller(self):
        report = scan(synthetic_module(), {'clock_nid': 'sceKernelGetProcessTime'})
        entry = report['timing_imports'][0]
        self.assertEqual(entry['stub'], '0x1020')
        self.assertEqual(entry['tail_wrapper_candidates'], [
            {'address': '0x1010', 'callers': [{'site': '0x1000', 'operation': 'call'}]}])

    def test_only_referenced_float_is_returned(self):
        report = scan(synthetic_module(), {'clock_nid': 'sceKernelGetProcessTime'})
        self.assertEqual(report['float_references'], [
            {'address': '0x2000', 'value': 'seconds_30hz',
             'sites': [{'site': '0x1030', 'operation': 'movss'}]}])

    def test_requested_reference_and_branch(self):
        report = scan(synthetic_module(), {}, (0x2000, 0x1010))
        self.assertEqual(report['requested_references'][0]['memory'][0]['site'], '0x1030')
        self.assertEqual(report['requested_references'][1]['direct_branches'][0]['site'], '0x1000')

    def test_rip_address_signs(self):
        self.assertEqual(rip_target(0x1000, 8, 'xmm0, dword ptr [rip - 0x20]'), 0xfe8)
        self.assertEqual(rip_target(0x1000, 8, 'xmm0, dword ptr [rip]'), 0x1008)
        self.assertIsNone(rip_target(0x1000, 8, '[rax + 0x20]'))

    def test_schedparam_is_not_tsc(self):
        self.assertIsNone(TIMING.search('scePthreadAttrSetschedparam'))
        self.assertIsNotNone(TIMING.search('sceKernelReadTsc'))


if __name__ == '__main__':
    unittest.main()
