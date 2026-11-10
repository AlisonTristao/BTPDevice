# BTPDevice

Biblioteca ESP-IDF que transforma um ESP32 num dispositivo **BTP** pronto para
conversar com o [TraceView](https://github.com/AlisonTristao/TraceView): telemetria,
terminal, comandos, criptografia e descoberta automática por nome. Você declara o
que o dispositivo mede e o que ele sabe fazer, e a BTPDevice cuida do resto.

> **Estado:** em implementação (versão 0.x). Links, terminal, comandos e tópicos
> já funcionam; a chave de criptografia e o sistema de arquivos chegam na Fase 5.
> O andamento está em [docs/PLANO.md](docs/PLANO.md) e os exemplos em
> [examples/](examples/).

## Para que serve

Todo projeto IoT que fala com o TraceView repete a mesma infraestrutura: escolher e
subir o link (Wi-Fi, Bluetooth, USB, ESP-NOW), montar o protocolo
[BTP](https://github.com/AlisonTristao/BTP), expor um shell
([TinyShell](https://github.com/AlisonTristao/TinyShell)), guardar a chave de
criptografia, publicar tópicos só quando alguém assina, reconectar, reportar erros.
A BTPDevice é essa infraestrutura escrita uma vez.

- **Um objeto, poucas linhas no `main`.** A parte densa (comandos, tópicos,
  armazenamento, rede) fica em arquivos de configuração, e há exemplos prontos
  para copiar.
- **Um nome, qualquer link.** O nome do dispositivo aparece no anúncio BLE, no
  mDNS (`<nome>.local`), na descrição da porta USB e no manifesto. O TraceView
  encontra o dispositivo pelo nome em qualquer um deles.
- **Telemetria sob demanda.** Você declara o tópico uma vez, e ele só é publicado
  enquanto o TraceView (ou outro cliente) está assinando, na taxa pedida.
- **Erros legíveis.** Cada falha sai no `ESP_LOG` com a causa e o que fazer, e o
  `begin()` devolve um código e uma mensagem.

## Como fica

```cpp
#include <BtpDevice.h>
#include "btp_device.h"     // nome, link, memória, rede
#include "btp_storage.h"    // LittleFS / SD, chave

void app_commands(btpdev::Commands& c);   // btp_commands.cpp
void app_topics(btpdev::Topics& t);       // btp_topics.cpp

BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    if (device.begin(app_commands, app_topics) != ESP_OK) {
        // a causa já saiu no log; device.last_error() tem o detalhe
    }
    while (true) {
        read_sensors();                   // o seu código
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

```cpp
// btp_topics.cpp: o que o TraceView pode plotar
void app_topics(btpdev::Topics& t) {
    t.topic("weather", btpdev::Hz(1))
     .f32("temperature", &temperature, "°C", -40.f, 85.f)
     .f32("humidity",    &humidity,    "%",    0.f, 100.f);
}

// btp_commands.cpp: o que dá para chamar pelo terminal do TraceView
void app_commands(btpdev::Commands& c) {
    c.module("relay", "Relé da bomba")
     .add("set", relay_set, "relay -set <0|1>");
}
```

De qualquer ponto do firmware:

```cpp
btpdev::printf("relé ligado\n");                             // terminal do TraceView
btpdev::log(btpdev::LogLevel::Warn, "bateria em %u%%", pct);  // painel de logs
device.publish("relay");                                      // amostra agora (evento)
device.run("relay -set 0");                                   // um comando, como se digitado
```

Sinais contínuos (áudio, vibração) vão num **stream**: cada bloco de amostras
chega ao TraceView uma vez, em ordem, com a taxa e um contador de blocos. O
widget Audio Spectrum do TraceView toca e mostra a FFT:

```cpp
t.stream("audio.pcm", 1024, "counts").range(-2048, 2047);   // btp_topics.cpp

device.write_stream("audio.pcm", block, 1024, 83333, esp_timer_get_time());
```

Um corpo que só o receptor sabe ler (uma imagem, uma grade compactada) vai num
tópico **bytes**: a função escreve o corpo e devolve o tamanho, ou 0 quando não
há nada novo ainda.

```cpp
t.bytes("camera.matrix", btpdev::Hz(30), 1900, copy_latest_frame).version(2);
```

## Chave e arquivos

Os links por rádio (TCP, BLE e ESP-NOW) só sobem com a chave do dispositivo.
Sem ela, o dispositivo cai para Serial e diz no log como provisionar.

```text
python tools/provision_key.py --password-e <senha E> --password-l <senha L>
# copie o bally.key gerado para a pasta data/ do projeto e:
pio run -t uploadfs      # data/ -> partição LittleFS (chave e wifi.txt)
```

No primeiro boot a chave vai para o NVS e o arquivo vira `bally.key.imported`.
No terminal do TraceView: `btp -key` (situação, só as tags públicas),
`btp -fs`, `btp -wifi`, `btp -link`. A mesma senha E é configurada no
TraceView para o dispositivo.

## Suporte

| | |
|---|---|
| Framework | ESP-IDF 6.0+ (componente) |
| Chips | ESP32-S3 primeiro. ESP32, ESP32-C3 e ESP32-C6 na sequência ([matriz de links](docs/PLANO.md#42-matriz-de-suporte)). |
| Links | Serial (USB ou UART), TCP (Wi-Fi), BLE, ESP-NOW (via dongle). **Um link por boot** na v1. |
| Dependências | [BTP](https://github.com/AlisonTristao/BTP), [TinyShell](https://github.com/AlisonTristao/TinyShell) |

## Documentação

- [docs/PLANO.md](docs/PLANO.md): decisões, arquitetura e fases de implementação.
- [CONTRIBUTING.md](CONTRIBUTING.md): como o código é organizado e como contribuir.
- [CHANGELOG.md](CHANGELOG.md): histórico de versões.

## Licença

[MIT](LICENSE) © 2026 Alison de Oliveira Tristão
