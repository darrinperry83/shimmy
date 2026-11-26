#!/usr/bin/env python3
"""Compatibility entry point for the separate hosted-core VM experiment."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent.parent / 'xnu-linux-abi'))
from stage_vm_variant import main
if __name__ == '__main__':main('core')
