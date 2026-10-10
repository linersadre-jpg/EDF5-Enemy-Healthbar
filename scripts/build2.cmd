@echo off
setlocal
rem Build the read-only P0 probe DLL (alternate/older link flag set).
rem Run from anywhere; ROOT resolves to the project root (parent of this script's dir).
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "SRC=%ROOT%\src"
set "OUT=%ROOT%\build"
if not exist "%OUT%" mkdir "%OUT%"
set "LOG=%OUT%\build.log"

set "VS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS set "VS=C:\BuildTools"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo vcvars64.bat failed & exit /b 1)

cl /nologo /std:c++17 /EHsc /W4 /wd4201 /utf-8 /permissive- /O2 /MT /DNDEBUG /c /I"%SRC%" /Fo"%OUT%\EDF5_Probe.obj" "%SRC%\EDF5_Probe.cpp" >"%LOG%" 2>&1 || (echo COMPILE FAILED & type "%LOG%" & exit /b 1)

link /nologo /DLL /DYNAMICBASE /NXCOMPAT /OUT:"%OUT%\EDF5EnemyHealthBarProbe.dll" /PDB:"%OUT%\EDF5EnemyHealthBarProbe.pdb" "%OUT%\EDF5_Probe.obj" >>"%LOG%" 2>&1 || (echo LINK FAILED & type "%LOG%" & exit /b 1)

echo BUILD OK
