# Copyright (C) 2026 IC Weather LLC
"""Fixed FD stimulus and explicitly non-successful diagnostic checkpoints."""
import json
import math

# A fixed 1 g/kg one-sided mixing-ratio ray. Its largest evaluated displacement
# is 32 g/kg on the retained 22-step grid; all components stay nonnegative.
TRACER_SCALE = 1.e-3


def density_scale(maximum_density):
    """0.1% of the base-coordinate scale, before per-element positivity caps."""
    if not math.isfinite(maximum_density) or maximum_density <= 0:
        raise RuntimeError("FD density scale requires finite positive base density")
    return 1.e-3 * maximum_density


def retain_progress(output, trials, *, failed=None):
    """A progress artifact cannot be mistaken for a completed FD qualification."""
    path = output.with_name(output.name + ".progress.json")
    payload = dict(status="failed" if failed is not None else "in_progress",
                   validated_curves=len(trials), curves=trials)
    if failed is not None:
        payload["failed_trial"] = failed
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)
