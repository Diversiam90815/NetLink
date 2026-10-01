import os
from datetime import datetime
from pathlib import Path

from .enums import Architecture, Configuration, Platform
from .utils import BuildUtils, working_directory
from .versioning import VersionManager
from .paths import *


class BuildRunner:
    def __init__(self, root_dir: Path, build_dir: Path, project_name: str) -> None:
        self.root_dir = root_dir
        self.build_dir = build_dir
        self.project_name = project_name

        self.version: str | None = None
        self.build_number: int = 0

        self.version_manager = VersionManager(CMAKE_FILE)

    # ---- Versioning ----
    def update_app_version(self) -> None:
        self.build_number = self.version_manager.get_build_number()
        self.version = self.version_manager.full_version()

    # ---- CMake / build ----
    def prepare_cmake_project(self, platform: Platform, architecture: Architecture) -> None:
        prepare_cmd = [
            "cmake",
            "-G", str(platform),
            "-S", str(self.root_dir),
            "-B", str(self.build_dir),
            f"-DNETLINK_BUILD_NUMBER={self.build_number}",
        ]
        if platform == Platform.VS2022 or platform == Platform.VS2026:
            prepare_cmd += ["-A", str(architecture)]

        BuildUtils.execute_command(
            prepare_cmd,
            f"CMake: Generate {platform} project",
        )

        # build backend in Release
        BuildUtils.execute_command(
            [
                "cmake",
                "--build", str(self.build_dir),
                "--config", str(Configuration.Release),
                "--parallel", "8",
            ],
            f"CMake: Build {self.project_name} v{self.version or 'unknown'} (Release)",
        )

        # build backend in Debug
        BuildUtils.execute_command(
            [
                "cmake",
                "--build", str(self.build_dir),
                "--config", str(Configuration.Debug),
                "--parallel", "8",
            ],
            f"CMake: Build {self.project_name} v{self.version or 'unknown'} (Debug)",
        )

        # install Release
        BuildUtils.execute_command(
            [
                "cmake",
                "--install", str(self.build_dir),
                "--config", str(Configuration.Release),
                "--prefix", str(CMAKE_INSTALL_DIR),
            ],
            f"CMake: Install {self.project_name} (Release)",
        )

        # install Debug
        BuildUtils.execute_command(
            [
                "cmake",
                "--install", str(self.build_dir),
                "--config", str(Configuration.Debug),
                "--prefix", str(CMAKE_INSTALL_DIR),
            ],
            f"CMake: Install {self.project_name} (Debug)",
        )

    def run_cpp_unit_tests(self, configuration: Configuration, build_dir, target) -> None:
        with working_directory(build_dir):
            BuildUtils.execute_command(
                [
                    "cmake",
                    "--build", str(build_dir),
                    "--config", str(configuration),
                    "--target", str(target),
                ],
                "CMake: Build C++ unit tests",
            )

            BuildUtils.execute_command(
                [
                    "ctest",
                    "--test-dir", str(build_dir),
                    "-C", str(configuration),
                    "--output-on-failure",
                ],
                "CMake: Running C++ unit tests",
            )

    def run_cpp_benchmarks(self, build_dir: Path, target: str, benchmark_filter: str | None = None, repetitions: int = 1) -> None:
        # Benchmarks always run in Release
        configuration = Configuration.Release

        BuildUtils.execute_command(
            [
                "cmake",
                "--build", str(build_dir),
                "--config", str(configuration),
                "--target", str(target),
            ],
            f"CMake: Build C++ benchmarks ({configuration})",
        )

        executable = self._find_executable(build_dir / "benchmarks", target, configuration)

        results_dir = build_dir / "benchmarks" / "results"
        results_dir.mkdir(parents=True, exist_ok=True)
        results_file = results_dir / f"{target}_{datetime.now():%Y%m%d_%H%M%S}.json"

        command = [
            str(executable),
            f"--benchmark_out={results_file}",
            "--benchmark_out_format=json",
        ]
        if benchmark_filter:
            command.append(f"--benchmark_filter={benchmark_filter}")
        if repetitions > 1:
            command += [f"--benchmark_repetitions={repetitions}", "--benchmark_report_aggregates_only=true"]

        BuildUtils.execute_streaming(command, "Running C++ benchmarks")
        print(f"\nResults written to {results_file}")

    @staticmethod
    def _find_executable(directory: Path, target: str, configuration: Configuration) -> Path:
        # Multi-config generators (Visual Studio) put binaries into a per-configuration folder, Ninja does not
        name = f"{target}.exe" if os.name == "nt" else target

        for candidate in (directory / str(configuration) / name, directory / name):
            if candidate.is_file():
                return candidate

        raise FileNotFoundError(f"{name} not found in {directory}")
