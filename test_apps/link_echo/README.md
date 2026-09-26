# link_echo: teste dos links em hardware

Cada frame BTP válido que chega pelo link é devolvido pelo mesmo link. O lado
do PC é o [`tools/link_echo_test.py`](../../tools/link_echo_test.py), que manda
frames de 0 a 1000 bytes de payload (com bytes `0x00` no meio, para exercitar o
COBS) e confere se cada um volta idêntico, medindo o tempo de ida e volta.

Placa de referência: ESP32-S3 DevKitC. **Os logs saem pela porta "UART"** (o
conversor USB-serial da placa). A porta "USB" nativa é o link Serial em teste.

```text
pip install pyserial bleak
```

## Serial (USB CDC)

```text
pio run -d test_apps/link_echo -e serial -t upload
python tools/link_echo_test.py serial COMx
```

`COMx` é a porta que aparece ao ligar a porta "USB" da placa. No Gerenciador
de Dispositivos ela deve aparecer com o nome `btpdev-echo`.

## TCP (Wi-Fi)

```text
set ECHO_WIFI_SSID=minha-rede
set ECHO_WIFI_PASSWORD=minha-senha
pio run -d test_apps/link_echo -e tcp -t upload
python tools/link_echo_test.py tcp btpdev-echo.local
```

No log: `connected to "minha-rede" as 192.168.x.y` e
`reachable as btpdev-echo.local:44300`. Se o mDNS não resolver no seu PC, use o
IP do log. Para testar a reconexão, desligue e religue o roteador: o log deve
mostrar `lost ... reconnecting` e depois `connected` de novo, sem reiniciar a
placa.

## BLE

```text
pio run -d test_apps/link_echo -e ble -t upload
python tools/link_echo_test.py ble btpdev-echo
```

## ESP-NOW

Só compila e sobe: o eco precisa do bally_dongle do outro lado, e isso é
testado na Fase 5. Configure o MAC do dongle em `menuconfig` > *Link echo*.

## O que conta como aprovado

- `lost 0, corrupt 0, unexpected 0` com o `--count 200` padrão;
- os mesmos números com `--count 2000 --max-payload 4000` (frames grandes);
- depois de fechar e abrir de novo o script, o eco continua funcionando (a
  sessão anterior não deixa lixo na próxima).

Anote no PR os tempos de ida e volta de cada link. Eles são a referência para
comparar com o bally_OS.
