#!/usr/bin/env python3

import argparse
import os
import shutil
import inspect
import vitis
from hsi import *

# =========================================================
# Command registry
# =========================================================

COMMANDS = {}

def build_command(func):
    """
    Decorator to register CLI commands automatically
    """
    name = func.__name__
    sig = inspect.signature(func)

    COMMANDS[name] = {
        "func": func,
        "signature": sig
    }

    return func


# =========================================================
# Helpers
# =========================================================



# =========================================================
# Commands
# =========================================================

@build_command
def create_hls_component_project(fpga_part: str):
    pass


@build_command
def run_synth():
    pass

@build_command
def run_csim():
    pass

@build_command
def run_cosim():
    pass

# =========================================================
# CLI builder (auto from signature)
# =========================================================

def build_parser():
    parser = argparse.ArgumentParser(description="Vitis CLI")
    subparsers = parser.add_subparsers(dest="command", required=True)

    for name, meta in COMMANDS.items():
        func = meta["func"]
        sig = meta["signature"]

        sub = subparsers.add_parser(name)

        for param_name, param in sig.parameters.items():
            param_type = param.annotation if param.annotation != inspect._empty else str

            arg_name = f"--{param_name}"

            if param.default == inspect._empty:
                # required argument
                sub.add_argument(arg_name, required=True, type=param_type)
            else:
                if param_type == bool:
                    # boolean flag
                    sub.add_argument(arg_name, action="store_true")
                else:
                    sub.add_argument(arg_name, default=param.default, type=param_type)

        sub.set_defaults(func=func)

    return parser


# =========================================================
# CLI entry point
# =========================================================

def main():
    parser = build_parser()
    args = parser.parse_args()

    cmd = args.func

    # Extract only function arguments
    func_args = {
        k: v for k, v in vars(args).items()
        if k not in ["func", "command"]
    }
    cmd(**func_args)


if __name__ == "__main__":
    main()