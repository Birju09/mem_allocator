from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps        
import os


class MemAllocatorConan(ConanFile):
    name = "mem_allocator"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "src/*", "tests/*"

    def requirements(self):
        self.requires("gtest/1.15.0")

    def build_requirements(self):
        self.tool_requires("ninja/1.12.1")

    def layout(self):
        # Use flat folder structure matching CMakePresets: build/{preset_name}/generators
        # The preset name comes from the -of argument or defaults to build folder name
        build_folder = os.environ.get("CONAN_BUILD_FOLDER", "build")
        self.folders.set_base_generators(f"{build_folder}/generators")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()
