#!/usr/bin/env python3
"""ADS60 bulk list DMA (R3_NATIVE_DOF_JOB=4) is byte- and observer-identical to per-element DMA."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ListBulkTests(unittest.TestCase):
    def test_bulk_matches_per_element(self):
        cc=shutil.which(os.environ.get('CC','clang'))
        if not cc:raise unittest.SkipTest('C compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/('list-bulk.exe' if os.name=='nt' else 'list-bulk')
            cmd=[cc,'-std=gnu17','-O2','-I',str(ROOT/'include'),'-I',str(ROOT/'runtime/spu'),'-I',str(ROOT/'runtime/platform'),str(ROOT/'runtime/spu/tests/test_mfc_list_bulk.c'),str(ROOT/'runtime/platform/win32_compat.c'),'-o',str(exe)]
            subprocess.run(cmd,check=True,capture_output=True,text=True)
            env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','YDKJ_','LBP_'))}
            p=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=120)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertIn('OK: bulk list DMA matches per-element',p.stdout)
            self.assertIn('OK: late bail-out is linear',p.stdout)
            self.assertIn('[r3-dof] bulk list DMA img=419 GET',p.stderr)
            self.assertIn('[r3-dof] bulk list DMA img=419 PUT',p.stderr)
if __name__=='__main__':unittest.main()