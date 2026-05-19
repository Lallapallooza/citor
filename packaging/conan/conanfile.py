# Conan 2.x recipe for citor. Header-library -- no compile, no copy_source.
#
# Usage (local recipe, ahead of ConanCenter acceptance):
#   conan create packaging/conan --version $(cz version --project)
#   conan install --requires=citor/$(cz version --project) --remote=mylocal
#
# After ConanCenter acceptance, consumers can do `conan install
# --requires=citor/<version>` against the default ConanCenter remote with no
# extra setup.

from pathlib import Path

from conan import ConanFile
from conan.tools.files import copy
from conan.tools.layout import basic_layout


class CitorConan(ConanFile):
    name = "citor"
    version = "0.4.5"
    license = "MIT"
    homepage = "https://github.com/Lallapallooza/citor"
    url = "https://github.com/Lallapallooza/citor"
    description = "Header-only C++20 thread pool tuned for sub-microsecond dispatch."
    topics = ("thread-pool", "concurrency", "cpp20", "header-only", "work-stealing")

    settings = "os", "arch", "compiler"
    package_type = "header-library"
    no_copy_source = True

    # The recipe lives at `packaging/conan/conanfile.py`, two levels below
    # the repo root. `exports_sources = "../include/*"` would silently strip
    # the leading `../` so we stage the tree explicitly through
    # `export_sources()` instead.
    def export_sources(self):
        src = Path(self.recipe_folder).resolve().parent.parent
        copy(self, "*.h", str(src / "include"), str(Path(self.export_sources_folder) / "include"))
        copy(
            self,
            "*.hpp",
            str(src / "single_include"),
            str(Path(self.export_sources_folder) / "single_include"),
        )
        copy(self, "LICENSE", str(src), str(self.export_sources_folder))

    def validate(self):
        os_name = str(self.settings.os)
        if os_name not in ("Linux", "Windows"):
            self.output.warning(
                f"citor is validated on Linux and Windows; "
                f"building on {os_name} is unsupported."
            )
        if str(self.settings.arch) not in ("x86_64",):
            self.output.warning(
                f"citor is currently x86_64-only; building on {self.settings.arch} is unsupported."
            )

    def package_id(self):
        # Header-only: package id is independent of compiler/build_type.
        self.info.clear()

    def layout(self):
        basic_layout(self, src_folder=".")

    def package(self):
        src = Path(self.source_folder)
        pkg = Path(self.package_folder)
        copy(self, "*.h", str(src / "include"), str(pkg / "include"))
        copy(self, "*.hpp", str(src / "single_include"), str(pkg / "include"))
        copy(self, "LICENSE", str(src), str(pkg / "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "citor")
        self.cpp_info.set_property("cmake_target_name", "citor::citor")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.system_libs = ["pthread"]
