@echo off
cd /d "%~dp0"
title SDR+Easy - servidor para o celular
echo.
echo  SDR+Easy - servidor para ouvir pelo celular
echo  -------------------------------------------
echo  1) Feche o SDR+Easy normal antes: o RTL-SDR so funciona em um programa por vez.
echo  2) No celular (mesma rede Wi-Fi): abra o SDR+Easy, va em Fonte (receptor),
echo     escolha "SDR++ Server", digite um destes enderecos e a porta 5259:
echo.
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do echo        %%a
echo.
echo  3) Na primeira vez o Windows pergunta sobre o firewall: permita em "Redes privadas".
echo  Para parar o servidor, feche esta janela.
echo.
sdrpp.exe --server --port 5259
