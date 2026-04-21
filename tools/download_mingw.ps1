$r = Invoke-RestMethod 'https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest'
$a = $r.assets | Where-Object { $_.name -match 'x86_64.*posix.*seh.*ucrt.*\.zip' } | Select-Object -First 1
if (-not $a) {
	Write-Host '[!] No matching asset found in latest WinLibs release'
	exit 1
}
Write-Host ("[*] Downloading $($a.name) ...")
Invoke-WebRequest $a.browser_download_url -OutFile 'tools\mingw64.zip'
Write-Host '[*] Download complete.'
