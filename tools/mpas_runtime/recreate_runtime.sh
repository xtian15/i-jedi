#!/bin/sh
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
  echo "usage: $0 TARGET_VENV WHEELHOUSE BASE_PYTHON [EXACT_HASHED_REQUIREMENTS [RUNTIME_RECEIPT]]" >&2
  exit 2
fi

target_venv=$1
wheelhouse=$2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
base_python=$3
requirements=${4:-$script_dir/runtime-linux-aarch64.requirements.txt}
receipt=${5:-$script_dir/runtime_receipt.json}
export PYTHONDONTWRITEBYTECODE=1
unset PYTHONPATH BASH_ENV

if [ -e "$target_venv" ]; then
  echo "target environment already exists: $target_venv" >&2
  exit 2
fi
if [ ! -d "$wheelhouse" ]; then
  echo "wheelhouse does not exist: $wheelhouse" >&2
  exit 2
fi
if [ ! -f "$requirements" ]; then
  echo "exact runtime requirements do not exist: $requirements" >&2
  exit 2
fi
"$base_python" - "$requirements" "$receipt" <<'PY'
import hashlib
import json
from pathlib import Path
import platform
import sys
lock, receipt = Path(sys.argv[1]), json.loads(Path(sys.argv[2]).read_bytes())
if hashlib.sha256(lock.read_bytes()).hexdigest() != receipt["requirements_sha256"]:
    raise SystemExit("runtime requirements differ from the declared artifact lock")
if (platform.python_version() != receipt["runtime"]["python"] or
        platform.machine() != receipt["runtime"]["machine"]):
    raise SystemExit("base interpreter does not match the declared Python/platform")
PY
"$base_python" -m venv "$target_venv"
"$target_venv/bin/python" -m pip install \
  --no-index --no-compile --find-links "$wheelhouse" --require-hashes -r "$requirements"

"$target_venv/bin/python" "$script_dir/verify_runtime_artifacts.py" \
  --requirements "$requirements" --wheelhouse "$wheelhouse"


"$target_venv/bin/python" - "$receipt" "$wheelhouse" "$script_dir" <<'PY'
import json
from pathlib import Path
import sys
sys.path.append(sys.argv[3])
sys.path.append(str(Path(sys.argv[3]).parents[1] / "test/mpas"))
from runtime_identity import verify_runtime
from direct_two_step import verify_installed_wheel
receipt_path, wheelhouse = map(Path, sys.argv[1:3])
receipt = json.loads(receipt_path.read_bytes())
identity = verify_runtime(receipt_path)
model = receipt["mpas_pytorch"]
wheel = wheelhouse / ("mpas_pytorch-" + model["version"] + "-py3-none-any.whl")
verify_installed_wheel(wheel, model["wheel_sha256"])
import mpas_pytorch
if mpas_pytorch.__version__ != model["version"]:
    raise SystemExit("installed model version differs from the exact wheel authority")
print(json.dumps(identity, sort_keys=True))
PY
"$target_venv/bin/python" -m pip check
