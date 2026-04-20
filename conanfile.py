from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


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
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()
