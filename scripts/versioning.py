import re
from pathlib import Path
from subprocess import check_output
from typing import Optional


class VersionManager:
    """Reads the project version.
    """

    def __init__(self, cmake_file: Path, version_var: str = "PROJECT_VERSION") -> None:
        self.cmake_file = cmake_file
        self.version_var = version_var

    def get_build_number(self) -> int:
        output = check_output(["git", "rev-list", "HEAD"]).decode("utf-8", "replace")
        return len(output.splitlines())

    def get_current_version(self) -> Optional[str]:
        pattern = re.compile(r"set\(" + re.escape(self.version_var) + r"\s+(\d+(?:\.\d+){1,3})\)")

        with self.cmake_file.open("r", encoding="utf-8") as f:
            for line in f:
                match = pattern.search(line)
                if match:
                    return match.group(1)
        return None

    def full_version(self) -> Optional[str]:
        """PROJECT_VERSION plus the build number, e.g. 0.2.0.189."""
        current = self.get_current_version()
        if not current:
            return None
        return f"{current}.{self.get_build_number()}"
