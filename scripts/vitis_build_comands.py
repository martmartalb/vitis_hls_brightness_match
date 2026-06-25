#!/usr/bin/env python3

import argparse
import configparser
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


def apply_cfg_file(client, workspace, name, cfg_file, syn_file, tb_file):
    cwd = os.getcwd() + '/'
    config = configparser.ConfigParser()
    config.read(cfg_file)

    ws_cfg = client.get_config_file(
        path=os.path.join(workspace, name, 'hls_config.cfg')
    )

    for key, value in config.defaults().items():
        ws_cfg.set_value(key=key, value=value)

    for section in config.sections():
        for key, value in config.items(section):
            if key in config.defaults():
                continue
            if key == 'syn.file':
                value = cwd + syn_file
            elif key == 'tb.file':
                value = cwd + tb_file
            ws_cfg.set_value(section=section, key=key, value=value)


# =========================================================
# Commands
# =========================================================

@build_command
def create_hls_component_project(workspace: str, name: str, cfg_file: str, syn_file: str, tb_file: str):
    client = vitis.create_client()
    client.set_workspace(path=workspace)

    comp_path = os.path.join(workspace, name)
    if os.path.exists(comp_path):
        client.delete_component(name=name)

    comp = client.create_hls_component(
        name=name,
        template='empty_hls_component'
    )

    apply_cfg_file(client, workspace, name, cfg_file, syn_file, tb_file)

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
