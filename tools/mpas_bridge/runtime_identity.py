"""Check the declared Python runtime; architecture is an input, not a waiver."""
import hashlib
import json
from pathlib import Path
import platform


def verify_runtime(receipt_path):
    import netCDF4
    import numpy
    import torch

    receipt_bytes = Path(receipt_path).read_bytes()
    receipt = json.loads(receipt_bytes)
    expected = receipt["runtime"]
    actual = dict(python=platform.python_version(), torch=torch.__version__,
                  numpy=numpy.__version__, netcdf4=netCDF4.__version__,
                  machine=platform.machine())
    if set(expected) != set(actual) or actual != expected:
        raise RuntimeError(f"Python runtime identity mismatch: expected {expected}, got {actual}")
    if torch.version.cuda is not None:
        raise RuntimeError("CPU qualification cannot use a CUDA-enabled Torch distribution")
    return dict(runtime=actual, runtime_receipt_sha256=hashlib.sha256(receipt_bytes).hexdigest())
