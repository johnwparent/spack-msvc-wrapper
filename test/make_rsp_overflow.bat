:: Copyright Spack Project Developers. See COPYRIGHT file for details.
::
:: SPDX-License-Identifier: (Apache-2.0 OR MIT)

:: Appends more input objects to an rsp file than fit on a Windows command
:: line, so that a link driven by that rsp cannot be passed on as arguments.
::
:: The objects are copies of a single translation unit with no external
:: symbols, so they can all be named as inputs without colliding with one
:: another and contribute nothing to the resulting binary. They live under a
:: directory whose name contains a space, and are named by absolute path.
::
:: Usage: make_rsp_overflow.bat <rsp file>
@echo off
setlocal

set "RSP=%~1"
if "%RSP%"=="" (
    echo Usage: make_rsp_overflow.bat ^<rsp file^>
    exit /b 1
)

set "PAD_DIR=padding objects for an input list longer than a windows command line accepts and therefore passed to the linker through a response file"
set "PAD_COUNT=220"

if not exist "%PAD_DIR%" mkdir "%PAD_DIR%"
> "%PAD_DIR%\pad.cxx" echo static int spack_padding = 0;
cl /nologo /c /EHsc "%PAD_DIR%\pad.cxx" /Fo"%PAD_DIR%\pad.obj"
if errorlevel 1 exit /b 1

for /L %%I in (1,1,%PAD_COUNT%) do (
    copy /y "%PAD_DIR%\pad.obj" "%PAD_DIR%\pad%%I.obj" >nul
    if errorlevel 1 exit /b 1
    >> "%RSP%" echo "%CD%\%PAD_DIR%\pad%%I.obj"
)
exit /b 0
