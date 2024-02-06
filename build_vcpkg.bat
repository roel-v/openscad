vcpkg install

mkdir build

rem --trace ^
cmake -B build -S . ^
    -DCMAKE_TOOLCHAIN_FILE=d:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DUSE_BUILTIN_OPENCSG=TRUE ^
    -DENABLE_CAIRO=FALSE ^
    -DCMAKE_PREFIX_PATH=e:\Dropbox\bin\win_flex_bison-2.5.25 ^
    -DCMAKE_EXE_LINKER_FLAGS="/manifest:no" ^
    -DCMAKE_MODULE_LINKER_FLAGS="/manifest:no" ^
    -DCMAKE_SHARED_LINKER_FLAGS="/manifest:no"

cmake --build build --config Debug
cmake --build build --config Release
