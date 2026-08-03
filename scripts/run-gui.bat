@echo off
rem Launch the GUI. Qt runtime DLLs are deployed next to the exe by the build,
rem so no extra PATH setup is needed. Pass --data <dir> to choose a data folder.
"%~dp0..\build\GMM.exe" %*
