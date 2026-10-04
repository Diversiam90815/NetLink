import argparse
from .enums import Architecture, Configuration, Platform


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build and deploy script for NetLink"
    )

    parser.add_argument(
        "-p", "--prepare",
        action="store_true",
        help="Prepare the project for the IDE without building."
    )

    parser.add_argument(
        "-b", "--build",
        action="store_true",
        help="Build the project after generating the build files."
    )

    parser.add_argument(
        "-pl", "--platform",
        default=Platform.Ninja,
        type=Platform,
        choices=list(Platform),
        help="Select your Platform/IDE. Defaults to Ninja."
    )

    parser.add_argument(
        "-a", "--architecture",
        default=Architecture.x64,
        type=Architecture,
        choices=list(Architecture),
        help="Select the architecture. Defaults to x64."
    )

    parser.add_argument(
        "-c", "--configuration",
        default=Configuration.Release,
        type=Configuration,
        choices=list(Configuration),
        help="Select the build configuration. Defaults to Release."
    )
    
    parser.add_argument(
        "-t", "--runtest",
        action="store_true",
        help="Build and run the CMake test suite."
    )

    parser.add_argument(
        "-bm", "--benchmark",
        action="store_true",
        help="Build the benchmarks in Release and run them. Results are also written as JSON to the build directory."
    )

    parser.add_argument(
        "--benchmark-filter",
        default=None,
        help="Only run benchmarks matching this regex, e.g. 'ReliableLink' or 'Engine_Throughput'."
    )

    parser.add_argument(
        "--benchmark-repetitions",
        type=int,
        default=1,
        help="Run every benchmark this many times and report mean, median, stddev and cv instead of single results."
    )
    return parser
