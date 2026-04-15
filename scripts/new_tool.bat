@echo off
setlocal enabledelayedexpansion

if "%~1"=="" (
    echo Usage: scripts\new_tool.bat ^<tool_name^> [description]
    echo Example: scripts\new_tool.bat http_fetch "Fetch a URL"
    exit /b 1
)

set "TOOL_NAME=%~1"
set "DESCRIPTION=%~2"
if not defined DESCRIPTION set "DESCRIPTION=%TOOL_NAME% tool"

set "SRC=runtime\tools\%TOOL_NAME%.c"

if exist "%SRC%" (
    echo [ERROR] %SRC% already exists
    exit /b 1
)

echo [NEW_TOOL] Creating %SRC% ...

REM Copy template and replace placeholders
powershell -Command "$content = Get-Content 'templates\tool_template.c' -Raw; $content = $content -replace '\{\{TOOL_NAME\}\}', '%TOOL_NAME%'; $content = $content -replace '\{\{DESCRIPTION\}\}', '%DESCRIPTION%'; Set-Content -Path '%SRC%' -Value $content -NoNewline"

if not exist "%SRC%" (
    echo [ERROR] Failed to create %SRC%
    exit /b 1
)

REM Register in agent.json (add to tools section)
powershell -Command "$json = Get-Content 'agent.json' -Raw | ConvertFrom-Json; if (-not $json.tools.PSObject.Properties.Name.Contains('%TOOL_NAME%')) { $json.tools | Add-Member -NotePropertyName '%TOOL_NAME%' -NotePropertyValue $true; $json | ConvertTo-Json -Depth 10 | Set-Content 'agent.json'; Write-Host '[NEW_TOOL] Registered %TOOL_NAME% in agent.json'; } else { Write-Host '[NEW_TOOL] %TOOL_NAME% already in agent.json'; }"

echo.
echo [NEW_TOOL] Created: %SRC%
echo [NEW_TOOL] Next steps:
echo   1. Edit %SRC% to implement your tool logic
echo   2. Run run.bat --build to build and load the new tool DLL
echo.

endlocal
