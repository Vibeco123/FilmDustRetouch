@echo off
setlocal
cd /d "%~dp0"

if exist "C:\mingw64\bin\g++.exe" set "PATH=C:\mingw64\bin;%PATH%"
set "CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=gcc"

where g++ >nul 2>nul
if errorlevel 1 goto no_compiler
where windres >nul 2>nul
if errorlevel 1 goto no_windres
where cargo >nul 2>nul
if errorlevel 1 goto no_cargo

if not exist "publish" mkdir "publish"

echo [1/3] Building Rust healing and PhotoCraft Remove library...
cargo build --release
if errorlevel 1 goto rust_failed
if not exist "target\release\film_dust_heal.dll" goto rust_failed
copy /Y "target\release\film_dust_heal.dll" "film_dust_heal.dll" >nul
if errorlevel 1 goto rust_failed

echo [2/3] Embedding application icon...
windres -i "FilmDustRetouch.rc" -o "FilmDustRetouch_res.o" --output-format=coff
if errorlevel 1 goto build_failed

echo [3/3] Compiling Film Dust Retouch...
g++ -std=c++17 -O2 -DNDEBUG -municode -mwindows -static -static-libgcc -static-libstdc++ -finput-charset=UTF-8 -fexec-charset=UTF-8 "FilmDustRetouch.cpp" "FilmDustRetouch_res.o" -o "publish\FilmDustRetouch.exe" -lgdiplus -lshell32 -lole32 -loleaut32 -lwindowscodecs -luuid -luser32 -lgdi32
if errorlevel 1 goto build_failed

if exist "FilmDustRetouch_res.o" del "FilmDustRetouch_res.o"
copy /Y "film_dust_heal.dll" "publish\film_dust_heal.dll" >nul
if errorlevel 1 goto build_failed

echo.
echo BUILD SUCCESSFUL
echo EXE: %CD%\publish\FilmDustRetouch.exe
echo DLL: %CD%\publish\film_dust_heal.dll
echo.
pause
exit /b 0

:no_cargo
echo ERROR: cargo was not found. Install Rust and make sure cargo is in PATH.
echo.
pause
exit /b 1

:no_compiler
echo ERROR: g++.exe was not found. Expected C:\mingw64\bin\g++.exe
echo.
pause
exit /b 1

:no_windres
echo ERROR: windres.exe was not found. It is included with MinGW-w64.
echo.
pause
exit /b 1

:rust_failed
echo.
echo RUST BUILD FAILED. Copy all text above and send it here.
echo The first build may download Rust crates from crates.io.
echo.
pause
exit /b 1

:build_failed
echo.
echo C++ BUILD FAILED. Copy all text above and send it here.
echo.
pause
exit /b 1
