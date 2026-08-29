// version.h - the ONLY place the version lives outside the build script.
//
// build.ps1 rewrites this file before every compile, so the version is never put
// on a command line: bash -> PowerShell -> g++ re-quotes and re-splits arguments
// (CI run #8 turned -DDSKV_VERSION=0.1.0.0 into "-DDSKV_VERSION= 0.1.0.0" and gcc
// read it as a number with four decimal points).  A file cannot be mangled.
// The default below is what you get when you build with build.cmd locally.
#pragma once
#define DSKV_VERSION_STR L"0.1.0"
