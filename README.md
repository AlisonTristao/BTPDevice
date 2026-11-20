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

Um trabalho longo mostra uma **barra de progresso** no terminal, redesenhada na
mesma linha enquanto o comando ainda roda (a resposta sai abaixo dela):

```
dev> sensor -calibrate 200
! calibrate  47% [=============>                ]
```

`btpdev::progress` tem o formato de um callback C de progresso
(`void (*)(void* ctx, uint8_t percent)`), então a função do trabalho não
precisa conhecer o BtpDevice nem a shell. Quem liga os dois é o registro do
comando:

```cpp
// calibrate.h: não inclui o BtpDevice
using ProgressFn = void (*)(void* ctx, uint8_t percent);
uint8_t calibrate(int samples, ProgressFn on_progress = nullptr, void* ctx = nullptr);

// btp_commands.cpp
c.module("sensor", "Sensor").add("calibrate", [](int samples) {
    return calibrate(samples, btpdev::progress, (void*)"calibrate");
});
```

Pode chamar com a frequência que quiser: só o último valor é guardado, o link
recebe no máximo ~10 redesenhos por segundo e o 100% sempre sai.

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

## Configurações persistentes

O que o app precisa ajustar em campo (um setpoint, um período, um pino) vira uma
configuração: fica no NVS, sobrevive a reboot e muda pelo terminal sem recompilar.

```cpp
void settings(btpdev::Settings& s) override {
    s.f32("setpoint", &setpoint_, 25.0f).range(0, 100).unit("°C");
    s.u32("period_ms", &period_ms_, 500).range(100, 60000);
    s.u8("led_pin", &pin_, 48).reboot();             // lido só no boot
    s.text("local", local_, sizeof(local_), "bancada");
    s.boolean("alarme", &alarme_, true).on_change([this] { atualiza_alarme(); });
}
```

A lib carrega os valores salvos **antes** de `commands()` e `topics()`, então as
variáveis já estão certas quando o seu código roda. No terminal:

```text
cfg -list                  valor, tipo, padrão e faixa de cada uma
cfg -set setpoint, 30      confere tipo e faixa, salva e chama o on_change
cfg -reset setpoint        volta ao padrão (cfg -reset all)
```

No código, `device.set_setting("setpoint", "30")` faz o mesmo que o `cfg -set`, e
`device.save_setting("setpoint")` salva um valor que o próprio app mudou na variável.
O nome vira a chave do NVS (até 15 caracteres). Um valor salvo com outro tipo ou fora
da faixa nova volta ao padrão, com aviso. `btp -factory_reset yes` mantém as
configurações; `all` apaga.

## Memória e PSRAM

Em placas com PSRAM (ESP32-S3 N8R8, N16R8...), ligue-a no `sdkconfig.defaults` e a lib
põe nela os buffers dela (`DeviceConfig::psram = Psram::Buffers`, o padrão). O maior
consumidor de RAM interna de um dispositivo TCP é o Wi-Fi, e só o sdkconfig o manda
para a PSRAM:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y                 # N8R8/N16R8; QUAD nas de PSRAM quad
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y   # buffers do Wi-Fi e do lwIP na PSRAM
```

Sem PSRAM, ou com ela, `WifiConfig::buffers = WifiBuffers::Lean` (o padrão) reserva só 6
buffers de recepção e transmite com buffers dinâmicos: com PSRAM ligada, o IDF reservaria
~51 KB de buffers estáticos. `WifiBuffers::Sdkconfig` volta ao que o sdkconfig diz.

`Psram::Stacks` manda também as stacks da task de comunicação e das tasks de link. Uma
task com a stack na PSRAM não pode escrever na flash; as da lib nunca escrevem, mas os
`fill()` dos tópicos rodam na task de comunicação, então com `Stacks` eles também não
podem gravar arquivo nem NVS. `Psram::Off` deixa tudo na RAM interna.

O cartão SD só entra no binário quando é usado: `.sd = {.driver = btpdev::sd_card, ...}`
no `StorageConfig`.

## Debug pelo serial

Com `debug_log = true`, o `begin()` descreve o boot no **console serial** (o
`ESP_LOG` de sempre: UART0 ou USB-JTAG). Isso nunca vai para o TraceView. Cada
etapa mostra o tempo desde o boot e quanto heap ela consumiu, e no fim sai a
memória e a stack de cada task:

```cpp
const btpdev::DeviceConfig device_config = {
    .name = "sensor",
    .debug_log = true,
};
```

```
I (312) btpdev: [  0.012] boot      BTPDevice 0.1.0, esp32s3, IDF v6.0.1, fw 1.2.0
I (313) btpdev: [  0.013] identity  source 3a1f22c0, boot #42, MAC 24:6f:28:aa:bb:cc
I (331) btpdev: [  0.031] storage   LittleFS /fs, SD -, app -                  heap   -4.1 KB
I (344) btpdev: [  0.044] buffers   preset Medium                              heap  -61.2 KB
I (420) btpdev: [  0.120] link      TCP started, sealed                        heap  -38.7 KB
I (451) btpdev: [  0.151] ready     in 151 ms, BTPDevice took 133.9 KB
I (451) btpdev: heap      182.4 KB free of 320.0 KB, min 180.9 KB, largest block 110.0 KB
I (451) btpdev: task              core prio  stack peak / total min free
I (451) btpdev: main                 0    1    2.3 / 3.5 KB       1.2 KB
I (451) btpdev: btpdev_comms         0   10    3.1 / 8.0 KB       4.9 KB
I (451) btpdev: btpdev_shell         -    9    1.4 / 8.0 KB       6.6 KB
I (451) btpdev: btpdev_tcp           0    5    2.2 / 6.0 KB       3.8 KB
```

- **`peak`** é o máximo de stack que a task já usou desde que nasceu. Se o
  `min free` chegar perto de zero, aumente a stack dela.
- **Todas as tasks do sistema** (wifi, tiT, ipc...) entram na tabela quando o
  projeto tem `CONFIG_FREERTOS_USE_TRACE_FACILITY`. Nas que não são da
  biblioteca, o tamanho total não é conhecido e aparece `-`.
- **O heap por etapa é uma estimativa:** outra task alocando ao mesmo tempo
  (o Wi-Fi, por exemplo) também entra na conta.
- **As etapas assíncronas** (scan do Wi-Fi, IP, `nome.local`) continuam saindo
  pelos logs normais da biblioteca, depois do `ready`.
- Com o link Serial **na mesma porta do console** (a UART0, ou a USB-Serial-JTAG
  de um S3/C3/C6), o `ESP_LOG` fica mudo enquanto o link roda e o debug some
  junto. Os logs continuam chegando ao TraceView por `btpdev::log()`.

O mesmo relatório de memória sai a qualquer hora pelo comando **`btp -mem`**,
no terminal do TraceView, com ou sem `debug_log`.

## Atualização pela rede (recovery)

Com uma das tabelas de [partitions/](partitions/), o app fica no `factory` e um
firmware pequeno de **recovery** (~0,7 MB) fica na `ota_0`. O `pio run -t upload`
grava os dois quando o `platformio.ini` tem uma linha a mais:

```ini
board_build.partitions = ../../partitions/btpdev_16mb.csv   ; 4, 8 ou 16 MB
extra_scripts = post:../../tools/pio_recovery.py
```

Para atualizar sem cabo, mande `btp -recovery` pelo terminal do TraceView. O
dispositivo reinicia no recovery, entra no mesmo Wi-Fi do app e espera o
firmware como `<nome>.local`:

```text
python tools/btp_ota.py estacao-quintal.local .pio/build/esp32-s3/firmware.bin
```

Ou pelo navegador: `http://<nome>.local/` abre uma página para escolher o `.bin`.
Se nenhuma rede conhecida responder, o recovery cria a própria, `btp-<nome>`:
conecte o celular nela e a página abre sozinha (ou abra `http://192.168.4.1/`).
A senha E é digitada na página e não sai do aparelho.

O upload é autenticado pela chave E (a senha nunca sai do PC). Sem upload, o
recovery volta ao app em 10 minutos. Um app que trava três vezes seguidas antes
de ficar estável cai sozinho no recovery (`DeviceConfig::recovery.crash_loop`).
`btp -factory_reset yes` apaga o app e o LittleFS e mantém chave e Wi-Fi;
`btp -factory_reset all` apaga também o NVS.
Para gravar por USB sem apertar BOOT, `btp -bootloader` reinicia no modo download
da ROM; sem gravação, a placa volta sozinha ao app em 30 s. Detalhes em
[docs/PLANO.md](docs/PLANO.md#414-recovery-e-ota) e no exemplo
[06_recovery_s3](examples/06_recovery_s3).

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
| Chips | ESP32-S3 primeiro. ESP32 clássico pelo link Serial na UART (conversor da placa). ESP32-C3 e ESP32-C6 na sequência ([matriz de links](docs/PLANO.md#42-matriz-de-suporte)). |
| Links | Serial (USB TinyUSB, USB-Serial-JTAG ou UART), TCP (Wi-Fi), BLE, ESP-NOW (via dongle). **Um link por boot** na v1. |
| Dependências | [BTP](https://github.com/AlisonTristao/BTP), [TinyShell](https://github.com/AlisonTristao/TinyShell) |

## Documentação

- [docs/PLANO.md](docs/PLANO.md): decisões, arquitetura e fases de implementação.
- [CONTRIBUTING.md](CONTRIBUTING.md): como o código é organizado e como contribuir.
- [CHANGELOG.md](CHANGELOG.md): histórico de versões.

## Licença

[MIT](LICENSE) © 2026 Alison de Oliveira Tristão
