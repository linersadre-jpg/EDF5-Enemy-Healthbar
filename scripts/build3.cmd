@echo off
setlocal
rem Build the EDF5 enemy health bar MOD -> build\EDF5EnemyHealthBar.dll
rem Run from anywhere; ROOT resolves to the project root (parent of this script's dir).
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "SRC=%ROOT%\src"
set "OUT=%ROOT%\build"
set "VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /std:c++17 /EHsc /W4 /wd4201 /utf-8 /permissive- /O2 /MT /DNDEBUG /I"%SRC%" /Fo"%OUT%\\" /c "%SRC%\EDF5EnemyHealthBar.cpp" "%SRC%\EHB_overlay.cpp"
if errorlevel 1 (
  echo COMPILE FAILED
  exit /b 1
)

link /nologo /DLL /DYNAMICBASE /NXCOMPAT /MAP:"%OUT%\EDF5EnemyHealthBar.map" /OUT:"%OUT%\EDF5EnemyHealthBar.dll" /PDB:"%OUT%\EDF5EnemyHealthBar.pdb" "%OUT%\EDF5EnemyHealthBar.obj" "%OUT%\EHB_overlay.obj" user32.lib gdi32.lib kernel32.lib
if errorlevel 1 (
  echo LINK FAILED
  exit /b 1
)
echo BUILD OK
exit /b 0
