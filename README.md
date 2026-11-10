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
  mDNS (`<nome>.local`), no hostname DHCP (a lista de clientes do roteador), na
  descrição da porta USB e no manifesto. O TraceView
  encontra o dispositivo pelo nome em qualquer um deles.
- **Telemetria sob demanda.** Você declara o tópico uma vez, e ele só é publicado
  enquanto o TraceView (ou outro cliente) está assinando, na taxa pedida.
- **Erros legíveis.** Cada falha sai no `ESP_LOG` com a causa e o que fazer, e o
  `begin()` devolve um código e uma mensagem.

## Como fica

O dispositivo inteiro cabe numa classe derivada de `btpdev::App`: quem ele é,
o que o terminal pode chamar e o que o TraceView pode plotar, com o estado como
membro da classe ([examples/03_relay_actuator](examples/03_relay_actuator)).

```cpp
#include <BtpDevice.h>

class BombaJardim : public btpdev::App {
public:
    btpdev::DeviceConfig config() const override {
        return {.name = "bomba-jardim", .memory = btpdev::Memory::Small};
    }
    void commands(btpdev::Commands& c) override {
        c.module("relay", "Relé da bomba")
         .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>");
    }
    void topics(btpdev::Topics& t) override {
        relay_ = t.topic("relay", btpdev::Hz(1)).boolean("on", &on_);
    }

private:
    uint8_t set(uint8_t on) {
        on_ = on != 0U;
        gpio_set_level(RELE, on_);
        device().publish(relay_);   // amostra agora, pelo handle do tópico
        return RESULT_OK;
    }
    bool on_ = false;
    btpdev::Topic relay_;
};

BombaJardim app;
BtpDevice device(app);

extern "C" void app_main() {
    device.begin();   // ESP_OK, ou a causa no log e em device.last_error()
}
```

A classe só é lida dentro do `begin()`, nunca num construtor, então a ordem dos
dois globais não importa. O estilo em arquivos separados continua valendo e é o
dos outros exemplos:

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

Toda declaração de tópico vira um `btpdev::Topic`. Com ele, `publish()`,
`subscribed()` e `write_stream()` não procuram o tópico pelo nome, e um nome
errado deixa de ser um `false` silencioso em tempo de execução:

```cpp
btpdev::Topic relay = t.topic("relay", btpdev::Hz(1)).boolean("on", &on);
device.publish(relay);
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

## Tarefas e núcleos

A biblioteca roda em duas tasks próprias, configuráveis em `DeviceConfig::comms`
(`TaskConfig`) e `DeviceConfig::shell` (`ShellConfig`):

| Task | Padrão | O que faz |
|---|---|---|
| `comms` | núcleo 0, prioridade 10 | Dona única do nó BTP: recebe, publica tópicos, envia. Fica no núcleo dos rádios. |
| `shell` | sem afinidade, uma prioridade abaixo de `comms` | Executa os comandos, um de cada vez. |

O núcleo 1 fica livre para o seu código. Um laço de controle fixado nele nunca é
interrompido pela biblioteca:

```cpp
const btpdev::DeviceConfig device_config = {
    .name  = "robo-bancada",
    .comms = {.core = 0, .period_ms = 2, .timer = true},  // passada exata a cada 2 ms
    .shell = {.core = 0},                                 // comandos pesados longe do núcleo 1
};

xTaskCreatePinnedToCore(control_loop, "control", 4096, nullptr, 20, nullptr, 1);
```

- **`period_ms` sem `timer`** é arredondado para ticks do FreeRTOS. Com o padrão
  do IDF (`CONFIG_FREERTOS_HZ=100`), `period_ms = 1` vale 10 ms. Com
  `.timer = true`, um `esp_timer` periódico só acorda a task, e o período fica exato.
- **Nada da biblioteca roda em interrupção.** Uma passada cifra, envia e pega
  locks. Para cadenciar por um timer de hardware, o timer notifica a task. O
  mesmo vale para o `write_stream()`: num callback de DMA do ADC/I2S, notifique
  uma task e chame `write_stream()` de lá.
- **Com `Threading::Manual`**, você chama `device.routine()` do seu laço. A primeira
  task que chama `routine()` ou `tick()` vira a dona, e uma chamada de outra task
  é recusada e registrada no log.
- **Variáveis ligadas por ponteiro** são lidas pela task `comms`, possivelmente no
  outro núcleo. `float` e inteiros de até 4 bytes alinhados são lidos de forma
  atômica. `f64` não tem forma ligada (8 bytes não são atômicos): use `.fill()`.

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
