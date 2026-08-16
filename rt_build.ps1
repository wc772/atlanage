$env:PATH = "C:\Users\31243\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;C:\Users\31243\AppData\Local\bin\NASM;" + $env:PATH
Set-Location "$PSScriptRoot"

& .\atlc.exe --asm self-hosted/atl_rt.at 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "atlc 编译失败" -ForegroundColor Red; exit 1 }

& python -c "import io, re; p='_atl_build.asm'; s=io.open(p,encoding='utf-8',newline='').read(); i=s.index('; ===== __atl_main ====='); j=s.index('section .data', i); s=s[:i]+s[j:]; s=re.sub(r'^global __atl_func_\d+[ \t]*\r?\n', '', s, flags=re.M); io.open(p,'w',encoding='utf-8',newline='').write(s)"
if ($LASTEXITCODE -ne 0) { Write-Host "壳删除失败" -ForegroundColor Red; exit 1 }
& python _rt_fix.py
if ($LASTEXITCODE -ne 0) { Write-Host "extern 去重失败" -ForegroundColor Red; exit 1 }
& nasm -f win64 _atl_build.asm -o atl_rt.obj 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "nasm 汇编失败" -ForegroundColor Red; exit 1 }
Write-Host "  atl_rt.obj 构建完成（$(Get-Item atl_rt.obj).Length 字节, nasm）" -ForegroundColor Green