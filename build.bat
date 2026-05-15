@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
cl /nologo /O2 /MT /LD /FeSpreadControlVC.asi SpreadControlVC.cpp /link kernel32.lib user32.lib
cl /nologo /O2 /MT /LD /FeSpreadControlVC.dll SpreadControlVC.cpp /link kernel32.lib user32.lib
cl /nologo /O2 /MT /FeInjectSpreadControlVC.exe InjectSpreadControlVC.cpp /link kernel32.lib user32.lib
