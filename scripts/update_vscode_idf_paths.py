#!/usr/bin/env python3
"""Update ESP-IDF VS Code extension paths for the local machine."""

import argparse
import json
import os
import re
import sys
from pathlib import Path


DEFAULT_IDF_VERSION = "6.0.1"
DEFAULT_PYTHON_ENV = "idf6.0_py3.14_env"


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_idf_path(home: Path) -> Path:
    return home / ".espressif" / f"v{DEFAULT_IDF_VERSION}" / "esp-idf"


def idf_version_from_path(idf_path: Path) -> str:
    version_file = idf_path / "tools" / "cmake" / "version.cmake"
    if not version_file.exists():
        return DEFAULT_IDF_VERSION

    values = {}
    for line in version_file.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\s*set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+([0-9]+)\)", line)
        if match:
            values[match.group(1)] = match.group(2)

    if {"MAJOR", "MINOR", "PATCH"}.issubset(values):
        return f"{values['MAJOR']}.{values['MINOR']}.{values['PATCH']}"
    return DEFAULT_IDF_VERSION


def find_python_env(tools_path: Path, idf_version: str) -> Path:
    major_minor = ".".join(idf_version.split(".")[:2])
    python_env_dir = tools_path / "python_env"
    candidates = sorted(python_env_dir.glob(f"idf{major_minor}_py*_env"), reverse=True)
    if candidates:
        return candidates[0]
    return python_env_dir / DEFAULT_PYTHON_ENV


def python_executable(python_env_path: Path) -> Path:
    python_path = python_env_path / "bin" / "python"
    if python_path.exists():
        return python_path
    return python_env_path / "bin" / "python3"


def replace_espressif_root(value: str, tools_path: Path) -> str:
    updated = value.replace("${userHome}/.espressif", str(tools_path))
    updated = re.sub(r"/Users/[^/:]+/\.espressif", str(tools_path), updated)
    updated = re.sub(r"/home/[^/:]+/\.espressif", str(tools_path), updated)
    return updated


def is_managed_path(entry: str) -> bool:
    managed_fragments = (
        "/esp-idf/components/espcoredump",
        "/esp-idf/components/partition_table",
        "/esp-idf/components/app_update",
        "/esp-idf/tools",
        "/python_env/idf",
    )
    return any(fragment in entry for fragment in managed_fragments)


def build_extra_path(
    existing_path: str,
    idf_path: Path,
    python_env_path: Path,
    tools_path: Path,
) -> str:
    replaced_path = replace_espressif_root(existing_path, tools_path)
    preserved_entries = [
        entry
        for entry in replaced_path.split(os.pathsep)
        if entry and not is_managed_path(entry)
    ]
    managed_entries = [
        idf_path / "components" / "espcoredump",
        idf_path / "components" / "partition_table",
        idf_path / "components" / "app_update",
        python_env_path / "bin",
        idf_path / "tools",
    ]

    all_entries = [str(entry) for entry in managed_entries] + preserved_entries
    deduped_entries = []
    for entry in all_entries:
        if entry not in deduped_entries:
            deduped_entries.append(entry)
    return os.pathsep.join(deduped_entries)


def keep_vscode_paths_portable(settings: dict, repo_root: Path, tools_path: Path) -> None:
    clangd_path = settings.get("clangd.path")
    if isinstance(clangd_path, str):
        settings["clangd.path"] = clangd_path.replace(
            str(tools_path),
            "${userHome}/.espressif",
        )

    clangd_arguments = settings.get("clangd.arguments")
    if isinstance(clangd_arguments, list):
        compile_commands_dir = f"--compile-commands-dir={repo_root / 'build'}"
        settings["clangd.arguments"] = [
            "--compile-commands-dir=${workspaceFolder}/build"
            if argument == compile_commands_dir
            else argument
            for argument in clangd_arguments
        ]


def validate_paths(
    idf_path: Path,
    tools_path: Path,
    python_path: Path,
    force: bool,
) -> None:
    missing = []
    if not (idf_path / "tools" / "cmake" / "project.cmake").exists():
        missing.append(f"ESP-IDF project.cmake: {idf_path / 'tools' / 'cmake' / 'project.cmake'}")
    if not tools_path.exists():
        missing.append(f"ESP-IDF tools path: {tools_path}")
    if not python_path.exists():
        missing.append(f"ESP-IDF Python executable: {python_path}")

    if missing and not force:
        print(
            "Refusing to update .vscode/settings.json because these paths were not found:",
            file=sys.stderr,
        )
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        print("Use --force to write the settings anyway.", file=sys.stderr)
        sys.exit(1)


def parse_args() -> argparse.Namespace:
    home = Path.home()
    parser = argparse.ArgumentParser(
        description="Rewrite ESP-IDF VS Code extension paths in .vscode/settings.json."
    )
    parser.add_argument(
        "--settings",
        type=Path,
        default=repository_root() / ".vscode" / "settings.json",
    )
    parser.add_argument(
        "--idf-path",
        type=Path,
        default=Path(os.environ.get("IDF_PATH", default_idf_path(home))),
    )
    parser.add_argument(
        "--tools-path",
        type=Path,
        default=Path(os.environ.get("IDF_TOOLS_PATH", home / ".espressif")),
    )
    parser.add_argument("--python-env-path", type=Path, default=None)
    parser.add_argument("--dry-run", action="store_true", help="Print the updated JSON without writing it.")
    parser.add_argument("--force", action="store_true", help="Write settings even if the detected paths do not exist.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    settings_path = args.settings.resolve()
    idf_path = args.idf_path.expanduser().resolve()
    tools_path = args.tools_path.expanduser().resolve()
    idf_version = idf_version_from_path(idf_path)
    if args.python_env_path:
        python_env_path = args.python_env_path.expanduser().resolve()
    else:
        python_env_path = find_python_env(tools_path, idf_version)
    python_path = python_executable(python_env_path)

    if not settings_path.exists():
        print(f"Settings file not found: {settings_path}", file=sys.stderr)
        return 1

    validate_paths(idf_path, tools_path, python_path, args.force)

    settings = json.loads(settings_path.read_text(encoding="utf-8"))
    settings["idf.espIdfPath"] = str(idf_path)
    settings["idf.toolsPath"] = str(tools_path)
    settings["idf.pythonInstallPath"] = str(python_path)
    settings["idf.currentSetup"] = str(idf_path)

    custom_extra_vars = settings.setdefault("idf.customExtraVars", {})
    custom_extra_vars["IDF_TARGET"] = custom_extra_vars.get("IDF_TARGET", "esp32s3")
    custom_extra_vars["IDF_PATH"] = str(idf_path)
    custom_extra_vars["IDF_TOOLS_PATH"] = str(tools_path)
    custom_extra_vars["ESP_IDF_VERSION"] = idf_version
    custom_extra_vars["IDF_PYTHON_ENV_PATH"] = str(python_env_path)
    custom_extra_vars["PYTHON"] = str(python_path)
    custom_extra_vars["PATH"] = build_extra_path(
        custom_extra_vars.get("PATH", ""),
        idf_path,
        python_env_path,
        tools_path,
    )
    keep_vscode_paths_portable(settings, repository_root(), tools_path)

    output = json.dumps(settings, indent=2) + "\n"
    if args.dry_run:
        print(output, end="")
        return 0

    settings_path.write_text(output, encoding="utf-8")
    print(f"Updated {settings_path}")
    print(f"ESP-IDF path: {idf_path}")
    print(f"ESP-IDF tools path: {tools_path}")
    print(f"ESP-IDF Python: {python_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
