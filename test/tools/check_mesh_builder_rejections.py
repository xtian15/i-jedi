#!/usr/bin/env python3
"""Reject the three measured Atlas ownership/index corruptions by cause."""
# Copyright (C) 2026 IC Weather LLC
# Licensed under the Apache License, Version 2.0.
import argparse
import os
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("mpi", "executable", "config", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--mpi-args", default="")
    args = parser.parse_args()
    command = [args.mpi, *shlex.split(args.mpi_args), "-n", "2",
               args.executable, args.config, args.output]
    expected = {
        "clear_ghost_flags": "ghost array and topology ownership disagree",
        "structured_as_global": "owned IDs are not a unique dense permutation",
        "shift_structured": "physical node shifted relative to independent",
    }
    for fault, cause in expected.items():
        env = dict(os.environ, IJEDI_CONTRACT_FAULT=fault)
        result = subprocess.run(command, env=env, text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        if result.returncode == 0 or cause not in result.stdout:
            raise RuntimeError(f"{fault}: wrong rejection\n{result.stdout}")
        print(f"{fault}: rejected for expected cause")


if __name__ == "__main__":
    main()
