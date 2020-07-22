# CLANG-FORMAT

Code formatting tool used in Tally is based on clang-format tool of LLVM suit. This tool is customized for coding standards and conventions of Tally.

## Compiling clang-format

### Pre-requisites for compiling clang-format

Please install latest 64 bit versions of the following pre-requisites before setting out to compile clang-format from source.

* CMake
* Git client
* Visual Studio 2019
* Python 3

### Steps for compiling clang-format tool

1. Clone the git repository from the tally clang-format repository:

    git clone https://tw-vishwamitra.tallysolutions.com/SharedProjects/TWPMT/_git/TWLLVMClangFormat (via Visual Studio 2019, Team Explorer)

2. Create a build directory (Eg. Build) in the repository root folder.

3. Open x64 Native tools Command Prompt for Visual Studio 2019

4. Change to build folder created in step 2.

5. Run the following cmake command to generate the Visual Studio 2019 solution file.

    cmake -Thost=x64 -G "Visual Studio 16 2019" -A x64 -DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra -DCMAKE_BUILD_TYPE=Release ..\llvm

6. Open the solution file generated in step 5 (LLVM.sln) in Visual Studio 2019.

7. Right click the clang-format project in Visual Studio 2019 solution explorer and select build.

8. clang-format.exe will be built in Release\bin directory of build folder.