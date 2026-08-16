$env:PATH = "C:\Users\31243\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;C:\Users\31243\AppData\Local\bin\NASM;" + $env:PATH
Set-Location "$PSScriptRoot"

function Invoke-Protected([string]$exe, [string]$argStr, [string]$outFile, [int]$secs = 900) {
    $p = Start-Process -FilePath $exe -ArgumentList $argStr -NoNewWindow -PassThru `
        -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"
    $deadline = (Get-Date).AddSeconds($secs)
    while (-not $p.HasExited) {
        if ((Get-Date) -gt $deadline) { $p.Kill(); Write-Host "KILLED: timeout" -ForegroundColor Red; return $false }
        try { $p.Refresh() } catch { break }
        if ($p.WorkingSet64 -gt (24GB)) { $p.Kill(); Write-Host "KILLED: mem>24GB" -ForegroundColor Red; return $false }
        Start-Sleep -Milliseconds 500
    }
    $p.WaitForExit()
    return $true
}

Write-Host "===== 步骤1: AT-V1.exe 自编译 -> _b7_self.asm =====" -ForegroundColor Yellow
$ok = Invoke-Protected ".\AT-V1.exe" "self-hosted/atlv1.at" "_b7_self.asm"
$selfAsmSize = if (Test-Path _b7_self.asm) { (Get-Item _b7_self.asm).Length } else { 0 }
if (-not $ok -or $selfAsmSize -lt 1000) {
    Write-Host "atlv1 自编译失败 (asm=${selfAsmSize}B)" -ForegroundColor Red
    Get-Content _b7_self.asm.err -TotalCount 5 -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "  自编译完成: asm=${selfAsmSize}B" -ForegroundColor Green

Write-Host "===== 步骤2: AT-V1.exe -obj 自研汇编器 -> _b7_self.obj =====" -ForegroundColor Yellow

& .\AT-V1.exe -obj _b7_self.asm "-out=_b7_self.obj"
if ($LASTEXITCODE -ne 0) { Write-Host "atlv1 -obj 失败" -ForegroundColor Red; exit 1 }
Write-Host "  -obj 完成: $((Get-Item _b7_self.obj).Length) B" -ForegroundColor Green

Write-Host "===== 步骤3: AT-V1.exe -link 自研链接器 -> AT-V1_self.exe =====" -ForegroundColor Yellow
& .\AT-V1.exe -link _b7_self.obj "-out=AT-V1_self.exe"
if ($LASTEXITCODE -ne 0) { Write-Host "atlv1 -link 失败" -ForegroundColor Red; exit 1 }
Write-Host "  AT-V1_self.exe 生成完成（全自研链第二代）" -ForegroundColor Green

Write-Host "===== 步骤4: 输出一致性比较（自举收敛） =====" -ForegroundColor Yellow
& .\AT-V1.exe _regression/t1_format.at | Out-File -FilePath "_b7_a.asm" -Encoding ascii
& .\AT-V1_self.exe _regression/t1_format.at | Out-File -FilePath "_b7_b.asm" -Encoding ascii
$fa = Get-Content -LiteralPath "_b7_a.asm" -Raw
$fb = Get-Content -LiteralPath "_b7_b.asm" -Raw
if ($fa -eq $fb) {
    Write-Host "  自举收敛：AT-V1.exe 与 AT-V1_self.exe 输出一致" -ForegroundColor Green
} else {
    Write-Host "  输出不一致！" -ForegroundColor Red
    exit 1
}
Write-Host "===== 完全自举验证完成 =====" -ForegroundColor Yellow