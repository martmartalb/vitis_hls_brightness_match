#!/usr/bin/env python3

import argparse
import os
import shutil
import inspect
import vitis

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

def get_component(workspace: str, name: str):
    client = vitis.create_client()
    client.set_workspace(path=workspace)
    comp = client.get_component(name=name)
    return comp


def parse_cfg_file(cfg_file):
    entries = []
    current_section = None
    with open(cfg_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1]
                continue
            if '=' in line:
                key, value = line.split('=', 1)
                entries.append((current_section, key.strip(), value.strip()))
    return entries


def apply_cfg_file(client, workspace, name, cfg_file):
    cwd = os.getcwd() + '/'
    entries = parse_cfg_file(cfg_file)

    ws_cfg = client.get_config_file(
        path=os.path.join(workspace, name, 'hls_config.cfg')
    )

    for section, key, value in entries:
        if key in ('syn.file', 'tb.file'):
            value = cwd + value

        if section is None:
            ws_cfg.set_value(key=key, value=value)
        else:
            ws_cfg.set_value(section=section, key=key, value=value)


# =========================================================
# Commands
# =========================================================

@build_command
def create_hls_component_project(workspace: str, name: str, cfg_file: str):
    client = vitis.create_client()
    client.set_workspace(path=workspace)

    comp_path = os.path.join(workspace, name)
    if os.path.exists(comp_path):
        client.delete_component(name=name)

    comp = client.create_hls_component(
        name=name,
        template='empty_hls_component'
    )

    apply_cfg_file(client, workspace, name, cfg_file)

    return comp


@build_command
def run_synth(workspace: str, name: str):
    comp = get_component(workspace, name)
    comp.run(operation='SYNTHESIS')


@build_command
def run_csim(workspace: str, name: str):
    comp = get_component(workspace, name)
    comp.run(operation='C_SIMULATION')


@build_command
def run_cosim(workspace: str, name: str):
    comp = get_component(workspace, name)
    comp.run(operation='CO_SIMULATION')


@build_command
def run_package(workspace: str, name: str):
    comp = get_component(workspace, name)
    comp.run(operation='PACKAGE')


# =========================================================
# CLI builder (auto from signature)
# =========================================================

def build_parser():
    parser = argparse.ArgumentParser(description="Vitis HLS Build Commands")
    subparsers = parser.add_subparsers(dest="command", required=True)

    for name, meta in COMMANDS.items():
        func = meta["func"]
        sig = meta["signature"]

        sub = subparsers.add_parser(name)

        for param_name, param in sig.parameters.items():
            param_type = param.annotation if param.annotation != inspect._empty else str

            arg_name = f"--{param_name}"

            if param.default == inspect._empty:
                sub.add_argument(arg_name, required=True, type=param_type)
            else:
                if param_type == bool:
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

    func_args = {
        k: v for k, v in vars(args).items()
        if k not in ["func", "command"]
    }
    cmd(**func_args)


if __name__ == "__main__":
    main()
